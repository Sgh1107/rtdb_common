/// W4 WAL 组提交单测（docs/08 §3-W4，docs/06 §3 注入矩阵）：
///  - WalRing 直测：记录编解码/扫描、CRC 撕裂检出、回绕垫片与 seq 连续性、
///    毒化标志持久化；
///  - 恢复重放：多次重开幂等、update/delete 往返；
///  - ExecuteBatch：三段式 seq 记账、运行期冲突整批拒绝（WAL 无 BEGIN）；
///  - 三档持久化：always 档同步水位、per_second 档后台节流；
///  - 错误注入：WAL 追加失败 / 索引发布失败 → 毒化拒写保读；
///  - 跨进程崩溃注入：子进程在 docs/06 §3 注入点 quick_exit(9)（kill -9
///    等价），父进程重开映像验证恢复语义（作废/收敛/自愈）。

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "infra/failpoint.hpp"
#include "infra/layout.hpp"
#include "infra/shm_region.hpp"
#include "platform/platform_process.hpp"
#include "rtdb/engine.hpp"
#include "rtdb/record_block.hpp"
#include "storage/group_commit.hpp"
#include "storage/wal_ring.hpp"
#include "tests/unit/spawn_self.hpp"

namespace {

namespace fs = std::filesystem;
using rtdb::ColumnValue;
using rtdb::DataType;
using rtdb::Err;
using rtdb::IsOk;
using rtdb::RecordBlock;
using rtdb::TableDef;
using rtdb::TableHandle;
namespace storage = rtdb::storage;
namespace failpoint = rtdb::failpoint;

/// 建表定义：id(i64,主键) val(i64)。
TableDef MakeDef(const std::string& name) {
    TableDef def;
    def.name = name;
    def.columns.push_back({"id", DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", DataType::kInt64, 0, true, false});
    return def;
}

std::vector<ColumnValue> Row(int64_t id, int64_t val) {
    return {ColumnValue::Int(id), ColumnValue::Int(val)};
}

void RemoveFile(const char* path) {
    std::error_code ec;
    fs::remove(path, ec);
}

// ===========================================================================
// WalRing 直测（绕开 Engine，直接驱动环数据结构）
// ===========================================================================

class WalRingTest : public ::testing::Test {
protected:
    void SetUp() override { RemoveFile(kImage); }
    void TearDown() override { RemoveFile(kImage); }

    /// 打开独立区域并创建新环（环所有权由 ring_ 持有）。
    storage::WalRing* MakeRegionAndRing() {
        rtdb::ShmRegionParams rp;
        rp.map_name = "walring_test";
        rp.file_path = kImage;
        rp.size_bytes = 1ull << 20;
        rp.mode = rtdb::OpenMode::OpenOrCreate;
        rp.role = rtdb::Role::Standalone;
        auto res = rtdb::ShmRegion::Open(rp);
        if (!res.IsOk()) return nullptr;
        region_ = res.TakeValue();
        bool created = false;
        auto ring_res = storage::WalRing::CreateOrAttach(region_->base_address(),
                                                         region_->allocator(),
                                                         region_->super_block(), &created);
        if (!ring_res.IsOk() || !created) return nullptr;
        ring_ = ring_res.TakeValue();
        return ring_.get();
    }

    /// 重新附着既有环（不新建）。
    storage::WalRing* ReattachRing(bool* out_created) {
        auto ring_res = storage::WalRing::CreateOrAttach(region_->base_address(),
                                                         region_->allocator(),
                                                         region_->super_block(), out_created);
        if (!ring_res.IsOk()) return nullptr;
        ring_ = ring_res.TakeValue();
        return ring_.get();
    }

    /// 记录区基址（测试直接改写字节制造撕裂/损坏）。
    uint8_t* AreaBytes() const {
        return reinterpret_cast<uint8_t*>(region_->base_address()) +
               region_->super_block()->wal_ring_off + sizeof(storage::shm_WalRingHeader);
    }

    struct Rec {
        uint64_t seq;
        uint8_t op;
        std::vector<uint8_t> payload;
    };

    std::vector<Rec> ScanAll(storage::WalRing* ring, uint64_t* last_seq) {
        std::vector<Rec> out;
        const Err e = ring->ScanValid(
            [&](const storage::WalRecordView& v) -> Err {
                Rec r;
                r.seq = v.seq;
                r.op = v.op;
                if (v.payload_len != 0 && v.payload != nullptr)
                    r.payload.assign(v.payload, v.payload + v.payload_len);
                out.push_back(std::move(r));
                return Err::kOk;
            },
            last_seq);
        EXPECT_TRUE(IsOk(e)) << rtdb::ErrorMessage(e);
        return out;
    }

    std::unique_ptr<rtdb::ShmRegion> region_;
    std::unique_ptr<storage::WalRing> ring_;
    static constexpr const char* kImage = "rtdb_gtest_walring.rtdb";
};

TEST_F(WalRingTest, RecordRoundtripAndCrcGate) {
    storage::WalRing* ring = MakeRegionAndRing();
    ASSERT_NE(ring, nullptr);

    uint64_t seq = 0;
    const uint8_t p1[2] = {0xAB, 0xCD};
    ASSERT_TRUE(IsOk(ring->Append(storage::kWalOpInsert, 7, 3, 2, p1, 2, &seq)));
    EXPECT_EQ(seq, 1u);
    ASSERT_TRUE(IsOk(ring->Append(storage::kWalOpUpdate, 7, 3, 4, p1, 2, &seq)));
    EXPECT_EQ(seq, 2u);
    uint8_t cnt[4];
    storage::EncodeU32LE(cnt, 5);
    ASSERT_TRUE(IsOk(ring->AppendMark(storage::kWalOpBatchBegin, cnt, 4, &seq)));
    EXPECT_EQ(seq, 3u);
    ASSERT_TRUE(IsOk(ring->AppendMark(storage::kWalOpBatchCommit, nullptr, 0, &seq)));
    EXPECT_EQ(seq, 4u);

    uint64_t last = 0;
    std::vector<Rec> recs = ScanAll(ring, &last);
    ASSERT_EQ(recs.size(), 4u);
    EXPECT_EQ(last, 4u);
    EXPECT_EQ(recs[0].seq, 1u);
    EXPECT_EQ(recs[0].op, storage::kWalOpInsert);
    EXPECT_EQ(recs[0].payload, std::vector<uint8_t>({0xAB, 0xCD}));
    EXPECT_EQ(recs[2].op, storage::kWalOpBatchBegin);
    EXPECT_EQ(storage::DecodeU32LE(recs[2].payload.data()), 5u);
    EXPECT_EQ(recs[3].op, storage::kWalOpBatchCommit);
    EXPECT_TRUE(recs[3].payload.empty());

    // CRC 门控：破坏首条记录载荷 → 扫描止步于其前，交付 0 条。
    // （AreaBytes() 即记录区起点，首记录无回绕，载荷位于偏移 56。）
    AreaBytes()[56] ^= 0xFF;  // 首记录 payload[0]
    last = 99;
    recs = ScanAll(ring, &last);
    EXPECT_TRUE(recs.empty());
    EXPECT_EQ(last, 0u);
}

TEST_F(WalRingTest, WrapPadAndSeqContinuity) {
    storage::WalRing* ring = MakeRegionAndRing();
    ASSERT_NE(ring, nullptr);

    // 变长载荷（0/4/8B 循环）迫使回绕垫片与裸回绕反复出现。
    uint64_t total = 0;
    for (int cycle = 0; cycle < 3; ++cycle) {
        uint64_t appended = 0;
        for (uint32_t len = 0;; len = (len + 4) % 12) {
            const uint64_t need = storage::WalRing::RecordBytes(len);
            if (!ring->HasRoomFor(need)) break;
            std::vector<uint8_t> payload(len, static_cast<uint8_t>(len));
            uint64_t seq = 0;
            ASSERT_TRUE(IsOk(ring->Append(storage::kWalOpInsert, 1, appended, 2, len != 0
                                                                                    ? payload.data()
                                                                                    : nullptr,
                                          len, &seq)));
            EXPECT_EQ(seq, ++total);
            ++appended;
        }
        // 单圈应填满大半个环（回绕路径确实被走到）。
        ASSERT_GT(appended, 500u) << "cycle " << cycle;

        // 扫描：seq 严格连续（含 pad 跳过）；起点 = 环内最老保留 seq。
        uint64_t expect_seq = ring->oldest_seq();
        uint64_t last = 0;
        std::vector<Rec> recs = ScanAll(ring, &last);
        ASSERT_EQ(recs.size(), appended);
        for (const Rec& r : recs) {
            EXPECT_EQ(r.seq, expect_seq++);
            EXPECT_EQ(r.op, storage::kWalOpInsert);
        }
        EXPECT_EQ(last, total);

        // 覆盖协议的环侧动作：丢弃保留段后可继续写。
        ring->DropAllRetained();
        EXPECT_TRUE(ring->HasRoomFor(storage::WalRing::RecordBytes(64)));
    }
}

/// P0-3：尾垫片回绕的落位约束——记录既放不下 [head, area) 的剩余连续区，
/// 回绕后又容身不了 [0, tail_pos) 时必须拒绝（kLimitExceeded），不得覆盖
/// 环内既有数据（旧判据只看总量，此窗口静默覆盖 [tail_pos, head_pos)）。
TEST_F(WalRingTest, LargeRecordWrapNeverOverwrites) {
    storage::WalRing* ring = MakeRegionAndRing();
    ASSERT_NE(ring, nullptr);
    const uint64_t area = ring->area_bytes();
    const uint64_t min_rec = storage::kWalMinRecordBytes;
    ASSERT_EQ(storage::WalRing::RecordBytes(0), min_rec);
    ASSERT_GT(area, 4 * min_rec);

    // 造状态：两条最小记录（head = tail = 120），丢弃全部保留段。
    uint64_t seq = 0;
    ASSERT_TRUE(IsOk(ring->Append(storage::kWalOpInsert, 1, 0, 2, nullptr, 0, &seq)));
    ASSERT_TRUE(IsOk(ring->Append(storage::kWalOpInsert, 1, 0, 2, nullptr, 0, &seq)));
    ring->DropAllRetained();
    const uint64_t tail = 2 * min_rec;

    // 待写记录 consumed = 3000：要求 head 落在 (area-3000, area-3000+tail]
    // —— 旧判据（总量 3000 ≤ area - pending）放行，垫片回绕后覆盖 [tail, head)。
    const uint64_t big_consumed = 3000;
    const uint64_t payload_len = static_cast<uint32_t>(big_consumed - min_rec);
    uint64_t head = tail;
    while (head <= area - big_consumed) head += min_rec;
    ASSERT_GT(head, area - big_consumed);
    ASSERT_LE(head, area - big_consumed + tail);
    for (uint64_t h = tail; h < head; h += min_rec) {
        ASSERT_TRUE(IsOk(ring->Append(storage::kWalOpInsert, 1, 0, 2, nullptr, 0, &seq)));
    }
    const uint64_t filled = (head - tail) / min_rec;
    EXPECT_EQ(seq, 2 + filled);

    std::vector<uint8_t> big(payload_len, 0x5A);
    const uint64_t big_bytes = storage::WalRing::RecordBytes(payload_len);
    EXPECT_EQ(big_bytes, big_consumed);
    EXPECT_FALSE(ring->HasRoomFor(big_bytes));  // 旧判据此处为 true
    EXPECT_EQ(ring->Append(storage::kWalOpInsert, 1, 0, 2, big.data(),
                           static_cast<uint32_t>(payload_len), nullptr),
              Err::kLimitExceeded);

    // 环内数据完好：seq 从 3 起严格连续。
    uint64_t last = 0;
    std::vector<Rec> recs = ScanAll(ring, &last);
    ASSERT_EQ(recs.size(), filled);
    EXPECT_EQ(recs.front().seq, 3u);
    EXPECT_EQ(last, 2 + filled);

    // 连续落位可达的小记录仍可写（拒绝不扩大化）。
    ASSERT_TRUE(IsOk(ring->Append(storage::kWalOpInsert, 1, 0, 2, nullptr, 0, &seq)));
    EXPECT_EQ(seq, 3 + filled);
    recs = ScanAll(ring, &last);
    EXPECT_EQ(recs.size(), filled + 1);
}

/// ScanRange（M3-W2 订阅增量面）：窗口交付/回调停止/预算截断/空区间/
/// 溢出判据。语义：回调返回 false → 当前记录不计入交付（可从该记录重扫）。
TEST_F(WalRingTest, ScanRangeWindowStopAndOverflow) {
    storage::WalRing* ring = MakeRegionAndRing();
    ASSERT_NE(ring, nullptr);
    uint64_t seq = 0;
    for (int i = 1; i <= 4; ++i) {
        const uint8_t p = static_cast<uint8_t>(i);
        ASSERT_TRUE(IsOk(ring->Append(storage::kWalOpInsert, 1, static_cast<uint64_t>(i), 2, &p, 1,
                                      &seq)));
    }
    ASSERT_EQ(seq, 4u);

    std::vector<uint64_t> got;
    const auto collect = [&got](const storage::WalRecordView& v) -> bool {
        got.push_back(v.seq);
        return true;
    };
    uint64_t last = 0;

    // 窗口 (1,3]：仅交付 2/3；游标推进到 3。
    EXPECT_TRUE(IsOk(ring->ScanRange(1, 3, collect, &last)));
    EXPECT_EQ(got, std::vector<uint64_t>({2u, 3u}));
    EXPECT_EQ(last, 3u);

    // 接续 (3,4]：不重复交付 3。
    got.clear();
    EXPECT_TRUE(IsOk(ring->ScanRange(last, 4, collect, &last)));
    EXPECT_EQ(got, std::vector<uint64_t>({4u}));
    EXPECT_EQ(last, 4u);

    // 回调 false：当前记录不计入交付（无缺漏，可重扫）。
    last = 0;
    EXPECT_TRUE(IsOk(ring->ScanRange(0, 4, [](const storage::WalRecordView&) { return false; },
                                     &last)));
    EXPECT_EQ(last, 0u);

    // 预算截断：交付前 2 条后主动停 —— 剩余由下一次窗口接续。
    got.clear();
    last = 0;
    EXPECT_TRUE(IsOk(ring->ScanRange(
        0, 4,
        [&got](const storage::WalRecordView& v) -> bool {
            if (got.size() >= 2) return false;
            got.push_back(v.seq);
            return true;
        },
        &last)));
    EXPECT_EQ(got, std::vector<uint64_t>({1u, 2u}));
    EXPECT_EQ(last, 2u);

    // 空区间 / 起点越过已写入上限：正常空交付（无溢出误报）。
    got.clear();
    last = 99;
    EXPECT_TRUE(IsOk(ring->ScanRange(4, 4, collect, &last)));
    EXPECT_EQ(last, 4u);
    EXPECT_TRUE(got.empty());
    EXPECT_TRUE(IsOk(ring->ScanRange(9, 20, collect, &last)));
    EXPECT_TRUE(got.empty());

    // 溢出判据：丢弃保留段后 from+1 < oldest_seq → kCursorOverflow。
    ring->DropAllRetained();
    EXPECT_EQ(ring->oldest_seq(), 5u);
    EXPECT_EQ(ring->ScanRange(0, 4, collect, &last), Err::kCursorOverflow);
    // 重置到最新（= oldest-1 起点不失真）：空环区间正常。
    EXPECT_TRUE(IsOk(ring->ScanRange(4, 4, collect, &last)));
}

/// ScanRange 跨回绕：seq 连续性与窗口上界截断（回绕后仍可增量续读）。
TEST_F(WalRingTest, ScanRangeAcrossWrapDeliversWindow) {
    storage::WalRing* ring = MakeRegionAndRing();
    ASSERT_NE(ring, nullptr);

    uint64_t total = 0;
    uint64_t appended = 0;
    for (uint32_t len = 0;; len = (len + 4) % 12) {
        const uint64_t need = storage::WalRing::RecordBytes(len);
        if (!ring->HasRoomFor(need)) break;
        std::vector<uint8_t> payload(len, static_cast<uint8_t>(len));
        uint64_t seq = 0;
        ASSERT_TRUE(IsOk(ring->Append(storage::kWalOpInsert, 1, appended, 2,
                                      len != 0 ? payload.data() : nullptr, len, &seq)));
        EXPECT_EQ(seq, ++total);
        ++appended;
    }
    ASSERT_GT(appended, 500u);
    const uint64_t oldest = ring->oldest_seq();
    EXPECT_EQ(oldest, 1u);

    std::vector<uint64_t> got;
    const auto collect = [&got](const storage::WalRecordView& v) -> bool {
        got.push_back(v.seq);
        return true;
    };
    uint64_t last = 0;

    // 全窗：oldest-1 → total 连续交付。
    EXPECT_TRUE(IsOk(ring->ScanRange(oldest - 1, total, collect, &last)));
    ASSERT_EQ(got.size(), appended);
    for (size_t i = 0; i < got.size(); ++i) EXPECT_EQ(got[i], oldest + i);
    EXPECT_EQ(last, total);

    // 上界截断：只交付前 100 条。
    got.clear();
    EXPECT_TRUE(IsOk(ring->ScanRange(oldest - 1, oldest + 99, collect, &last)));
    ASSERT_EQ(got.size(), 100u);
    EXPECT_EQ(got.front(), oldest);
    EXPECT_EQ(got.back(), oldest + 99);
    EXPECT_EQ(last, oldest + 99);
}

/// ScanRange 撕裂尾：CRC 不匹配止步于其前（与 ScanValid 同语义）。
TEST_F(WalRingTest, ScanRangeTornTailStopsCleanly) {
    storage::WalRing* ring = MakeRegionAndRing();
    ASSERT_NE(ring, nullptr);
    uint64_t seq = 0;
    for (int i = 0; i < 3; ++i) {
        const uint8_t p[2] = {static_cast<uint8_t>(i), 0x11};
        ASSERT_TRUE(IsOk(ring->Append(storage::kWalOpInsert, 1, 0, 2, p, 2, &seq)));
    }
    // 破坏第 2 条载荷首字节（记录区起点 + 首条长度 + 头长）。
    const uint64_t rec1 = storage::WalRing::RecordBytes(2);
    AreaBytes()[rec1 + storage::kWalRecHdrBytes] ^= 0xFF;

    std::vector<uint64_t> got;
    uint64_t last = 0;
    EXPECT_TRUE(IsOk(ring->ScanRange(0, 3,
                                     [&got](const storage::WalRecordView& v) -> bool {
                                         got.push_back(v.seq);
                                         return true;
                                     },
                                     &last)));
    EXPECT_EQ(got, std::vector<uint64_t>({1u}));
    EXPECT_EQ(last, 1u);
}

TEST_F(WalRingTest, PoisonPersistsAcrossAttach) {
    storage::WalRing* ring = MakeRegionAndRing();
    ASSERT_NE(ring, nullptr);
    EXPECT_FALSE(ring->poisoned());
    ring->MarkPoisoned();
    uint64_t seq = 0;
    EXPECT_EQ(ring->Append(storage::kWalOpInsert, 1, 0, 2, nullptr, 0, &seq), Err::kPoisoned);

    // 重新附着（同映像）：毒化标志持久于环头。
    bool created = true;
    storage::WalRing* again = ReattachRing(&created);
    ASSERT_NE(again, nullptr);
    EXPECT_FALSE(created);
    EXPECT_TRUE(again->poisoned());
}

// ===========================================================================
// 引擎级：恢复重放幂等 / 批量 / 三档持久化 / 毒化
// ===========================================================================

class WalEngineTest : public ::testing::Test {
protected:
    void SetUp() override { RemoveFile(kImage); }
    void TearDown() override { failpoint::ClearAll(); RemoveFile(kImage); }

    rtdb::Options Opts(rtdb::FsyncPolicy fsync = rtdb::FsyncPolicy::PerSecond) const {
        rtdb::Options opts;
        opts.instance = "wal_engine_test";
        opts.path = kImage;
        opts.initial_size_bytes = 2ull << 20;
        opts.role = rtdb::Role::Standalone;
        opts.wal_fsync = fsync;
        return opts;
    }

    static constexpr const char* kImage = "rtdb_gtest_wal_engine.rtdb";
};

TEST_F(WalEngineTest, ReplayIdempotentAcrossReopens) {
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
        auto eng = open.TakeValue();
        auto t = eng->CreateTable(MakeDef("t"));
        ASSERT_TRUE(t.IsOk());
        for (int64_t i = 1; i <= 3; ++i) ASSERT_TRUE(eng->Insert(t.Value(), Row(i, i * 10)).IsOk());
        EXPECT_EQ(eng->CommittedSeq(), 4);  // DDL + 3 行
    }
    // 第一次重开：重放 (0, 4]，状态一致。
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        EXPECT_EQ(eng->CommittedSeq(), 4);
        auto t_res = eng->OpenTable("t");
        ASSERT_TRUE(t_res.IsOk());
        auto got = eng->GetByKey(t_res.Value(), ColumnValue::Int(2));
        ASSERT_TRUE(got.IsOk());
        EXPECT_EQ(got.Value()[1].as_int(), 20);
    }
    // 第二次重开：重放幂等（不重复计数、不重复插入）。
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        EXPECT_EQ(eng->CommittedSeq(), 4);
        auto t_res = eng->OpenTable("t");
        ASSERT_TRUE(t_res.IsOk());
        auto t = t_res.Value();
        EXPECT_TRUE(eng->ContainsKey(t, ColumnValue::Int(3)).Value());
        EXPECT_EQ(eng->GetByKey(t, ColumnValue::Int(4)).Error(), Err::kNotFound);
    }
}

TEST_F(WalEngineTest, UpdateDeleteReplayRoundtrip) {
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        auto t_res = eng->CreateTable(MakeDef("t"));
        ASSERT_TRUE(t_res.IsOk());
        auto t = t_res.Value();
        ASSERT_TRUE(eng->Insert(t, Row(1, 1)).IsOk());
        ASSERT_TRUE(eng->Insert(t, Row(2, 2)).IsOk());
        ASSERT_TRUE(eng->UpdateByKey(t, ColumnValue::Int(1), Row(1, 11)).IsOk());
        ASSERT_TRUE(eng->DeleteByKey(t, ColumnValue::Int(2)).IsOk());
        EXPECT_EQ(eng->CommittedSeq(), 5);  // DDL + 2 插 + 1 改 + 1 删
    }
    for (int reopen = 0; reopen < 2; ++reopen) {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        auto t_res = eng->OpenTable("t");
        ASSERT_TRUE(t_res.IsOk());
        auto t = t_res.Value();
        auto got = eng->GetByKey(t, ColumnValue::Int(1));
        ASSERT_TRUE(got.IsOk());
        EXPECT_EQ(got.Value()[1].as_int(), 11);
        EXPECT_EQ(eng->GetByKey(t, ColumnValue::Int(2)).Error(), Err::kNotFound);
        EXPECT_EQ(eng->CommittedSeq(), 5);
    }
}

TEST_F(WalEngineTest, BatchAtomicAndSeqAccounting) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    ASSERT_EQ(eng->CommittedSeq(), 1);  // DDL

    std::vector<rtdb::Engine::BatchOp> batch;
    for (int64_t i = 100; i <= 104; ++i) {
        rtdb::Engine::BatchOp op;
        op.type = rtdb::Engine::BatchOp::Type::kInsert;
        op.table = t;
        op.row = Row(i, i);
        batch.push_back(std::move(op));
    }
    ASSERT_TRUE(eng->ExecuteBatch(batch).IsOk());
    // BEGIN + 5 op + COMMIT = 7 个 seq。
    EXPECT_EQ(eng->CommittedSeq(), 8);
    for (int64_t i = 100; i <= 104; ++i)
        EXPECT_TRUE(eng->ContainsKey(t, ColumnValue::Int(i)).Value());

    // 混合批：update + delete。
    std::vector<rtdb::Engine::BatchOp> mixed;
    rtdb::Engine::BatchOp upd;
    upd.type = rtdb::Engine::BatchOp::Type::kUpdate;
    upd.table = t;
    upd.row = Row(100, 999);
    mixed.push_back(std::move(upd));
    rtdb::Engine::BatchOp del;
    del.type = rtdb::Engine::BatchOp::Type::kDelete;
    del.table = t;
    del.row = {ColumnValue::Int(101)};
    mixed.push_back(std::move(del));
    ASSERT_TRUE(eng->ExecuteBatch(mixed).IsOk());
    EXPECT_EQ(eng->CommittedSeq(), 12);  // BEGIN + 2 op + COMMIT = 4
    EXPECT_EQ(eng->GetByKey(t, ColumnValue::Int(100)).Value()[1].as_int(), 999);
    EXPECT_EQ(eng->GetByKey(t, ColumnValue::Int(101)).Error(), Err::kNotFound);
}

TEST_F(WalEngineTest, BatchConflictAbortsCleanly) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    ASSERT_TRUE(eng->Insert(t, Row(10, 1)).IsOk());
    const int64_t committed_before = eng->CommittedSeq();

    std::vector<rtdb::Engine::BatchOp> batch;
    rtdb::Engine::BatchOp ok_op;
    ok_op.type = rtdb::Engine::BatchOp::Type::kInsert;
    ok_op.table = t;
    ok_op.row = Row(20, 2);
    batch.push_back(std::move(ok_op));
    rtdb::Engine::BatchOp dup;
    dup.type = rtdb::Engine::BatchOp::Type::kInsert;
    dup.table = t;
    dup.row = Row(10, 3);  // 主键冲突
    batch.push_back(std::move(dup));
    rtdb::Engine::BatchOp tail;
    tail.type = rtdb::Engine::BatchOp::Type::kInsert;
    tail.table = t;
    tail.row = Row(30, 4);
    batch.push_back(std::move(tail));

    EXPECT_EQ(eng->ExecuteBatch(batch).Error(), Err::kAlreadyExists);
    EXPECT_EQ(eng->CommittedSeq(), committed_before);  // 零副作用
    EXPECT_EQ(eng->ContainsKey(t, ColumnValue::Int(20)).Value(), false);
    EXPECT_EQ(eng->ContainsKey(t, ColumnValue::Int(30)).Value(), false);

    // WAL 无该批痕迹：重开后仍不存在。
    eng.reset();
    auto open2 = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open2.IsOk());
    auto eng2 = open2.TakeValue();
    auto t2_res = eng2->OpenTable("t");
    ASSERT_TRUE(t2_res.IsOk());
    auto t2 = t2_res.Value();
    EXPECT_EQ(eng2->ContainsKey(t2, ColumnValue::Int(20)).Value(), false);
    EXPECT_EQ(eng2->CommittedSeq(), committed_before);
}

TEST_F(WalEngineTest, BatchEmptyRejected) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    EXPECT_EQ(open.Value()->ExecuteBatch({}).Error(), Err::kInvalidArgument);
}

TEST_F(WalEngineTest, AlwaysTierFlushSynchronous) {
    auto open = rtdb::Engine::Open(Opts(rtdb::FsyncPolicy::Always));
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    ASSERT_TRUE(eng->Insert(t, Row(1, 1)).IsOk());
    // always 档：提交返回前完成全映像 flush。
    EXPECT_GE(eng->FlushedSeq(), eng->CommittedSeq());
    EXPECT_GT(eng->FlushedSeq(), 0);
}

TEST_F(WalEngineTest, PerSecondBackgroundFlush) {
    auto open = rtdb::Engine::Open(Opts(rtdb::FsyncPolicy::PerSecond));
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    ASSERT_TRUE(eng->Insert(t, Row(1, 1)).IsOk());
    EXPECT_GT(eng->CommittedSeq(), 0);
    // 后台节流线程一个周期内推进水位。
    std::this_thread::sleep_for(std::chrono::milliseconds(1600));
    EXPECT_GE(eng->FlushedSeq(), eng->CommittedSeq());
}

TEST_F(WalEngineTest, AppendFailurePoisonsAndPersists) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();

    failpoint::Arm("wal.append.fail", failpoint::Mode::kError);
    EXPECT_EQ(eng->Insert(t, Row(1, 1)).Error(), Err::kIoError);
    EXPECT_EQ(eng->Insert(t, Row(2, 2)).Error(), Err::kPoisoned);  // 拒写
    EXPECT_EQ(eng->GetByKey(t, ColumnValue::Int(1)).Error(), Err::kNotFound);  // 保读

    // 毒化持久化：重开后依然拒写。
    eng.reset();
    auto open2 = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open2.IsOk());
    auto eng2 = open2.TakeValue();
    auto t2_res = eng2->OpenTable("t");
    ASSERT_TRUE(t2_res.IsOk());
    auto t2 = t2_res.Value();
    EXPECT_EQ(eng2->Insert(t2, Row(3, 3)).Error(), Err::kPoisoned);
    EXPECT_TRUE(eng2->ContainsKey(t2, ColumnValue::Int(3)).Value() == false);
}

TEST_F(WalEngineTest, IndexPublishFailurePoisons) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();

    failpoint::Arm("wal.index.insert", failpoint::Mode::kError);
    // 应用阶段失败：数据页回滚、错误上抛、引擎毒化（WAL 已先行）。
    EXPECT_EQ(eng->Insert(t, Row(1, 1)).Error(), Err::kOutOfMemory);
    EXPECT_EQ(eng->GetByKey(t, ColumnValue::Int(1)).Error(), Err::kNotFound);
    EXPECT_EQ(eng->Insert(t, Row(2, 2)).Error(), Err::kPoisoned);
}

/// A1（P0-1）：批前 WAL 预留——整批字节先于任何写入核对容量；
///  - 超出记录区容量：拒批（kLimitExceeded）、零 WAL 副作用、引擎不毒化；
///  - 环近满但批可容身：批前一次性腾环（flush + checkpoint + 丢弃已
///    提交段），批内绝不丢弃未提交前缀。重开验证数据完整。
///
/// 数值：环 64KB → 记录区 65,472B；批字节 = BEGIN 64 + COMMIT 60 +
/// N×84（2 列 i64 的行记录 24B + 记录头 60B）。N=777 → 65,392（可容）；
/// N=778 → 65,476（超容）。
TEST_F(WalEngineTest, BatchWalReserveBoundaryAndDrain) {
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    TableDef def = MakeDef("t");
    def.row_cap = 2048;
    auto t_res = eng->CreateTable(def);
    ASSERT_TRUE(t_res.IsOk()) << rtdb::ErrorMessage(t_res.Error());
    auto t = t_res.Value();

    // RecordBlock 零拷贝：列数据须在 UpsertBatch 调用期间存活（同作用域）。
    std::vector<int64_t> ids, vals;
    auto fill = [&](size_t n, int64_t first) {
        ids.resize(n);
        vals.resize(n);
        for (size_t i = 0; i < n; ++i) {
            ids[i] = first + static_cast<int64_t>(i);
            vals[i] = static_cast<int64_t>(i);
        }
    };

    // ① 超容批：拒批、零副作用（无行、无提交推进）、后续可写（未毒化）。
    fill(778, 1);
    {
        RecordBlock blk("t");
        blk.AppendColumn("id", ids).AppendColumn("val", vals);
        ASSERT_TRUE(blk.IsValid());
        EXPECT_EQ(eng->UpsertBatch(blk).Error(), Err::kLimitExceeded);
    }
    EXPECT_TRUE(eng->ScanAll(t).Value().empty());
    EXPECT_EQ(eng->CommittedSeq(), 1);  // 仅建表 DDL

    // ② 边界内批（777 行）：成功。
    fill(777, 1);
    {
        RecordBlock blk("t");
        blk.AppendColumn("id", ids).AppendColumn("val", vals);
        ASSERT_TRUE(eng->UpsertBatch(blk).IsOk());
    }
    EXPECT_EQ(eng->ScanAll(t).Value().size(), 777u);

    // ③ 环已近满（② 后剩余 < 84B）：600 行批走批前腾环，成功。
    fill(600, 778);
    {
        RecordBlock blk("t");
        blk.AppendColumn("id", ids).AppendColumn("val", vals);
        ASSERT_TRUE(eng->UpsertBatch(blk).IsOk());
    }
    EXPECT_EQ(eng->ScanAll(t).Value().size(), 1377u);

    // 重开：腾环丢弃段由 checkpoint 兜底，批数据完整。
    eng.reset();
    auto open2 = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open2.IsOk());
    auto eng2 = open2.TakeValue();
    auto t2_res = eng2->OpenTable("t");
    ASSERT_TRUE(t2_res.IsOk());
    auto t2 = t2_res.Value();
    EXPECT_EQ(eng2->ScanAll(t2).Value().size(), 1377u);
    auto got = eng2->GetByKey(t2, ColumnValue::Int(1377));
    ASSERT_TRUE(got.IsOk()) << rtdb::ErrorMessage(got.Error());
    EXPECT_EQ(got.Value()[1].as_int(), 599);
}

/// A2（P0-2）：发布点（行数据 + 主键索引）之后的应用失败——已发布行保持
/// 可读（含变长内容、引用其变长块），引擎毒化（WAL 已先行）；重开后重放
/// 幂等跳过已发布行、数据完好，不得因回收已发布行的变长块而损坏。
TEST_F(WalEngineTest, SecIdxFailureAfterPublishPreservesRow) {
    TableDef def;
    def.name = "t";
    def.columns.push_back({"id", DataType::kInt64, 0, false, false});
    def.columns.push_back({"val", DataType::kInt64, 0, true, false});
    def.columns.push_back({"note", DataType::kUtf8Var, 0, true, false});
    def.row_cap = 64;
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(def);
    ASSERT_TRUE(t_res.IsOk()) << rtdb::ErrorMessage(t_res.Error());
    auto t = t_res.Value();
    rtdb::SecondaryIndexDef idx;
    idx.name = "by_val";
    idx.column_index = 1;
    idx.unique = false;
    ASSERT_TRUE(eng->CreateSecondaryIndex(t, idx).IsOk());
    ASSERT_TRUE(eng->Insert(t, {ColumnValue::Int(1), ColumnValue::Int(10),
                                ColumnValue::Text("hello")})
                    .IsOk());

    failpoint::Arm("wal.secidx.after_publish", failpoint::Mode::kError);
    EXPECT_EQ(eng->Insert(t, {ColumnValue::Int(2), ColumnValue::Int(20),
                              ColumnValue::Text("world")})
                  .Error(),
              Err::kOutOfMemory);
    // 拒写保读：已发布行可读（变长内容来自其仍持有的块）。
    auto got = eng->GetByKey(t, ColumnValue::Int(2));
    ASSERT_TRUE(got.IsOk()) << rtdb::ErrorMessage(got.Error());
    EXPECT_EQ(got.Value()[2].as_bytes(), "world");
    EXPECT_EQ(eng->Insert(t, {ColumnValue::Int(3), ColumnValue::Int(30),
                              ColumnValue::Text("x")})
                  .Error(),
              Err::kPoisoned);

    eng.reset();
    // 重开：行 2 数据完好、二级索引自愈重建；毒化持久（写仍被拒）。
    auto open2 = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open2.IsOk());
    auto eng2 = open2.TakeValue();
    auto t2_res = eng2->OpenTable("t");
    ASSERT_TRUE(t2_res.IsOk());
    auto t2 = t2_res.Value();
    got = eng2->GetByKey(t2, ColumnValue::Int(2));
    ASSERT_TRUE(got.IsOk()) << rtdb::ErrorMessage(got.Error());
    EXPECT_EQ(got.Value()[1].as_int(), 20);
    EXPECT_EQ(got.Value()[2].as_bytes(), "world");
    EXPECT_TRUE(eng2->GetByKey(t2, ColumnValue::Int(1)).IsOk());
    auto q = eng2->GetBySecondary(t2, "by_val", ColumnValue::Int(20));
    ASSERT_TRUE(q.IsOk()) << rtdb::ErrorMessage(q.Error());
    ASSERT_EQ(q.Value().size(), 1u);
    EXPECT_EQ(q.Value()[0][0].as_int(), 2);
    EXPECT_EQ(eng2->Insert(t2, {ColumnValue::Int(3), ColumnValue::Int(30),
                                ColumnValue::Text("x")})
                  .Error(),
              Err::kPoisoned);
}

// ===========================================================================
// 跨进程崩溃注入（docs/06 §3 注入矩阵，W4 覆盖前四点）
// ===========================================================================

class WalCrashRecoveryTest : public ::testing::Test {
protected:
    void SetUp() override { RemoveFile(Image()); }
    void TearDown() override { RemoveFile(Image()); }

    const char* Image() const { return "rtdb_gtest_wal_crash.rtdb"; }

    /// 以注入规格拉起子进程，期望其在注入点 quick_exit(9)。
    void RunChildToCrash(const std::string& failpoint_spec) {
        const std::string exe = rtdb::platform::HostExecutablePath();
        if (exe.empty()) GTEST_SKIP() << "当前平台无法定位自身可执行文件";
        rtdb_test::SetEnvVar("RTDB_FAILPOINTS", failpoint_spec.c_str());
        const auto out = rtdb_test::SpawnSelf(exe, {rtdb_test::kWalCrashFlag, Image()});
        rtdb_test::UnsetEnvVar("RTDB_FAILPOINTS");
        ASSERT_TRUE(out.spawned) << "子进程派生失败";
        ASSERT_EQ(out.exit_code, failpoint::kCrashExitCode)
            << "子进程应死于注入点 " << failpoint_spec;
    }

    rtdb::Options Opts() const {
        rtdb::Options opts;
        opts.instance = "wal_crash_parent";
        opts.path = Image();
        opts.initial_size_bytes = 2ull << 20;
        opts.role = rtdb::Role::Standalone;
        return opts;
    }
};

/// 注入点"Insert 前"：什么都没发生，重启后可写。
TEST_F(WalCrashRecoveryTest, CrashBeforeInsert) {
    RunChildToCrash("wal.insert.before=crash");
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    auto t = eng->OpenTable("t");
    ASSERT_TRUE(t.IsOk());                                  // 建表已提交
    EXPECT_EQ(eng->GetByKey(t.Value(), ColumnValue::Int(1)).Error(), Err::kNotFound);
    EXPECT_EQ(eng->CommittedSeq(), 1);                      // 仅 DDL
    ASSERT_TRUE(eng->Insert(t.Value(), Row(1, 10)).IsOk()); // 引擎可写
}

/// 注入点"WAL 追加中"：撕裂记录止步扫描，重启后自愈可写。
TEST_F(WalCrashRecoveryTest, CrashMidAppendSelfHeals) {
    RunChildToCrash("wal.append.mid=crash");
    {
        auto open = rtdb::Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        auto t_res = eng->OpenTable("t");
        ASSERT_TRUE(t_res.IsOk());
        auto t = t_res.Value();
        EXPECT_EQ(eng->GetByKey(t, ColumnValue::Int(1)).Error(), Err::kNotFound);
        EXPECT_EQ(eng->CommittedSeq(), 1);  // 撕裂记录不可信，止步于 DDL
        ASSERT_TRUE(eng->Insert(t, Row(1, 10)).IsOk());  // 覆写撕裂区，自愈
    }
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    auto got = eng->GetByKey(t, ColumnValue::Int(1));
    ASSERT_TRUE(got.IsOk());
    EXPECT_EQ(got.Value()[1].as_int(), 10);
    EXPECT_EQ(eng->CommittedSeq(), 2);
}

/// 注入点"索引更新一半"：数据已发布、索引缺失 → 重放补发布。
TEST_F(WalCrashRecoveryTest, CrashMidApplyCompletesPublish) {
    RunChildToCrash("wal.apply.mid=crash");
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    auto got = eng->GetByKey(t, ColumnValue::Int(1));
    ASSERT_TRUE(got.IsOk()) << "重放应补发布主键索引";
    EXPECT_EQ(got.Value()[1].as_int(), 10);
    EXPECT_EQ(eng->CommittedSeq(), 2);
    EXPECT_EQ(eng->GetByKey(t, ColumnValue::Int(2)).Error(), Err::kNotFound);
}

/// 注入点"commit 标记前"：变更已应用、水位未推进 → 重放对账后推进。
TEST_F(WalCrashRecoveryTest, CrashBeforeCommitAdvancesWatermark) {
    RunChildToCrash("wal.commit.before=crash");
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    ASSERT_TRUE(eng->GetByKey(t, ColumnValue::Int(1)).IsOk());
    EXPECT_EQ(eng->CommittedSeq(), 2);
}

/// 注入点"批 WAL 阶段中断"：无 COMMIT → 恢复整批作废。
TEST_F(WalCrashRecoveryTest, CrashBatchWalMidDiscardsBatch) {
    RunChildToCrash("wal.batch.wal_mid=crash");
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    for (int64_t i = 1; i <= 5; ++i) {
        auto got = eng->GetByKey(t, ColumnValue::Int(i));
        ASSERT_TRUE(got.IsOk());
        EXPECT_EQ(got.Value()[1].as_int(), i * 10);
    }
    for (int64_t i = 100; i <= 105; ++i)
        EXPECT_EQ(eng->GetByKey(t, ColumnValue::Int(i)).Error(), Err::kNotFound) << i;
    EXPECT_EQ(eng->CommittedSeq(), 6);  // DDL + 5 行；批整体作废
    ASSERT_TRUE(eng->Insert(t, Row(200, 1)).IsOk());  // 引擎可写
}

/// 注入点"批应用阶段中断"：有 COMMIT → 重放整批收敛。
TEST_F(WalCrashRecoveryTest, CrashBatchMidApplyRedoesAll) {
    RunChildToCrash("wal.batch.mid=crash");
    auto open = rtdb::Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->OpenTable("t");
    ASSERT_TRUE(t_res.IsOk());
    auto t = t_res.Value();
    for (int64_t i = 1; i <= 5; ++i) {
        auto got = eng->GetByKey(t, ColumnValue::Int(i));
        ASSERT_TRUE(got.IsOk());
        EXPECT_EQ(got.Value()[1].as_int(), i * 10);
    }
    for (int64_t i = 100; i <= 105; ++i) {
        auto got = eng->GetByKey(t, ColumnValue::Int(i));
        ASSERT_TRUE(got.IsOk()) << i;
        EXPECT_EQ(got.Value()[1].as_int(), i);
    }
    EXPECT_EQ(eng->CommittedSeq(), 14);  // DDL + 5 行 + BEGIN + 6 op + COMMIT
}

}  // namespace
