/// W2 嵌入订阅单测（docs/13 §3-D1/§5-W2）：
///  - 增量交付：insert/update/delete 事件内容、seq 严格递增、游标推进；
///  - 过滤器：仅交付订阅表（未订阅表的记录仍推进游标，允许重复不允许缺漏）；
///  - 原子批边界：ExecuteBatch 提交后整批可见（不交付未提交前缀）；
///  - 溢出与重同步：游标落后于环回收窗口 → kCursorOverflow → ResetTo；
///  - 等待语义：无变更超时空批、跨线程写入的唤醒交付（无丢失/无重复）；
///  - 扩展列：事件行携带该次写入的 ext 列值（未携带列不产出）；
///  - 拒绝面：表不存在 / Reader 主角色（未挂载环）。

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rtdb/change_cursor.hpp"
#include "rtdb/engine.hpp"

namespace {

namespace fs = std::filesystem;
using rtdb::ChangeCursor;
using rtdb::ChangeEvent;
using rtdb::ColumnValue;
using rtdb::DataType;
using rtdb::Engine;
using rtdb::Err;
using rtdb::TableDef;
using rtdb::TableHandle;

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

/// 扩展列定义助手（M1：定长 ≤8B 标量默认值）。
rtdb::ColumnDef ExtCol(const char* name, rtdb::DataType type, bool nullable, bool has_default,
                       int64_t def_int) {
    rtdb::ColumnDef c;
    c.name = name;
    c.type = type;
    c.nullable = nullable;
    c.has_default = has_default;
    c.default_value.int_value = def_int;
    return c;
}

void RemoveFile(const char* path) {
    std::error_code ec;
    fs::remove(path, ec);
}

class SubscribeTest : public ::testing::Test {
protected:
    void SetUp() override { RemoveFile(kImage); }
    void TearDown() override { RemoveFile(kImage); }

    rtdb::Options Opts(rtdb::Role role = rtdb::Role::Standalone) const {
        rtdb::Options o;
        o.instance = "w2_sub_test";
        o.path = kImage;
        o.initial_size_bytes = 4ull << 20;
        o.role = role;
        return o;
    }

    static constexpr const char* kImage = "rtdb_gtest_w2_sub.rtdb";
};

/// insert/update/delete 三种事件的内容与游标推进。
TEST_F(SubscribeTest, InsertUpdateDeleteDelivered) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    auto t = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t.IsOk());
    const TableHandle table = t.Value();

    auto sub = eng->Subscribe({"t"});
    ASSERT_TRUE(sub.IsOk()) << rtdb::ErrorMessage(sub.Error());
    ChangeCursor cur = sub.TakeValue();
    const uint64_t base = cur.Position();
    EXPECT_EQ(base, static_cast<uint64_t>(eng->CommittedSeq()));
    EXPECT_TRUE(cur.valid());

    ASSERT_TRUE(eng->Insert(table, Row(1, 100)).IsOk());
    auto b1 = cur.Next(0);
    ASSERT_TRUE(b1.IsOk()) << rtdb::ErrorMessage(b1.Error());
    ASSERT_EQ(b1->events.size(), 1u);
    const ChangeEvent& e1 = b1->events[0];
    EXPECT_EQ(e1.kind, ChangeEvent::Kind::kInsert);
    EXPECT_EQ(e1.table, "t");
    EXPECT_GT(e1.table_id, 0);
    EXPECT_EQ(e1.seq, base + 1);
    EXPECT_EQ(b1->last_seq, e1.seq);
    ASSERT_EQ(e1.row.size(), 2u);
    EXPECT_EQ(e1.row[0].as_int(), 1);
    EXPECT_EQ(e1.row[1].as_int(), 100);

    ASSERT_TRUE(eng->UpdateByKey(table, ColumnValue::Int(1), Row(1, 200)).IsOk());
    auto b2 = cur.Next(0);
    ASSERT_TRUE(b2.IsOk()) << rtdb::ErrorMessage(b2.Error());
    ASSERT_EQ(b2->events.size(), 1u);
    EXPECT_EQ(b2->events[0].kind, ChangeEvent::Kind::kUpdate);
    EXPECT_EQ(b2->events[0].seq, base + 2);
    EXPECT_EQ(b2->events[0].row[1].as_int(), 200);

    ASSERT_TRUE(eng->DeleteByKey(table, ColumnValue::Int(1)).IsOk());
    auto b3 = cur.Next(0);
    ASSERT_TRUE(b3.IsOk());
    ASSERT_EQ(b3->events.size(), 1u);
    EXPECT_EQ(b3->events[0].kind, ChangeEvent::Kind::kDelete);
    EXPECT_EQ(b3->events[0].seq, base + 3);
    EXPECT_TRUE(b3->events[0].row.empty());
    EXPECT_EQ(b3->events[0].key.as_int(), 1);

    // 追平后空批，游标 = committed。
    auto b4 = cur.Next(0);
    ASSERT_TRUE(b4.IsOk());
    EXPECT_TRUE(b4->empty());
    EXPECT_EQ(cur.Position(), static_cast<uint64_t>(eng->CommittedSeq()));
    EXPECT_EQ(b4->last_seq, cur.Position());
}

/// 过滤器：仅交付订阅表；未订阅表的记录仍推进游标（无缺漏、可复查）。
TEST_F(SubscribeTest, FilterOnlyDeliversSubscribedTable) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto ta = eng->CreateTable(MakeDef("a"));
    ASSERT_TRUE(ta.IsOk());
    auto tb = eng->CreateTable(MakeDef("b"));
    ASSERT_TRUE(tb.IsOk());
    const TableHandle a = ta.Value();
    const TableHandle b = tb.Value();

    auto sub = eng->Subscribe({"a"});
    ASSERT_TRUE(sub.IsOk()) << rtdb::ErrorMessage(sub.Error());
    ChangeCursor cur = sub.TakeValue();

    // 先写 b（订阅表之外的记录），后写 a：游标须跳过 b 并交付 a。
    ASSERT_TRUE(eng->Insert(b, Row(7, 70)).IsOk());
    ASSERT_TRUE(eng->Insert(a, Row(1, 10)).IsOk());
    ASSERT_TRUE(eng->UpdateByKey(a, ColumnValue::Int(1), Row(1, 11)).IsOk());

    auto batch = cur.Next(0);
    ASSERT_TRUE(batch.IsOk()) << rtdb::ErrorMessage(batch.Error());
    ASSERT_EQ(batch->events.size(), 2u);
    EXPECT_EQ(batch->events[0].kind, ChangeEvent::Kind::kInsert);
    EXPECT_EQ(batch->events[0].table, "a");
    EXPECT_EQ(batch->events[0].row[0].as_int(), 1);
    EXPECT_EQ(batch->events[1].kind, ChangeEvent::Kind::kUpdate);
    EXPECT_EQ(batch->events[1].row[1].as_int(), 11);
    // 游标已越过 b 的记录（追平 committed）。
    EXPECT_EQ(cur.Position(), static_cast<uint64_t>(eng->CommittedSeq()));
}

/// 原子批：COMMIT 后整批交付、seq 连续；提交前不交付任何前缀。
TEST_F(SubscribeTest, BatchDeliveredAfterCommit) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t.IsOk());
    const TableHandle table = t.Value();

    auto sub = eng->Subscribe({"t"});
    ASSERT_TRUE(sub.IsOk());
    ChangeCursor cur = sub.TakeValue();

    // 提交前：无任何可见变更。
    auto pre = cur.Next(0);
    ASSERT_TRUE(pre.IsOk());
    EXPECT_TRUE(pre->empty());

    std::vector<Engine::BatchOp> ops;
    for (int64_t i = 1; i <= 3; ++i) {
        Engine::BatchOp op;
        op.type = Engine::BatchOp::Type::kInsert;
        op.table = table;
        op.row = Row(i, i * 10);
        ops.push_back(std::move(op));
    }
    ASSERT_TRUE(eng->ExecuteBatch(ops).IsOk());

    auto batch = cur.Next(0);
    ASSERT_TRUE(batch.IsOk()) << rtdb::ErrorMessage(batch.Error());
    ASSERT_EQ(batch->events.size(), 3u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(batch->events[i].kind, ChangeEvent::Kind::kInsert);
        EXPECT_EQ(batch->events[i].row[0].as_int(), static_cast<int64_t>(i) + 1);
        EXPECT_EQ(batch->events[i].row[1].as_int(), (static_cast<int64_t>(i) + 1) * 10);
        if (i != 0) EXPECT_EQ(batch->events[i].seq, batch->events[i - 1].seq + 1);
    }
    // 批内操作 seq 连续；游标推进越过 COMMIT 记录。
    EXPECT_EQ(cur.Position(), static_cast<uint64_t>(eng->CommittedSeq()));
    EXPECT_GT(cur.Position(), batch->events[2].seq);
}

/// 无变更：等待至多 timeout 后返回空批（游标不动）。
TEST_F(SubscribeTest, TimeoutYieldsEmptyBatch) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t.IsOk());

    auto sub = eng->Subscribe({"t"});
    ASSERT_TRUE(sub.IsOk());
    ChangeCursor cur = sub.TakeValue();
    const uint64_t pos = cur.Position();

    const auto t0 = std::chrono::steady_clock::now();
    auto batch = cur.Next(40);
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0)
                                .count();
    ASSERT_TRUE(batch.IsOk()) << rtdb::ErrorMessage(batch.Error());
    EXPECT_TRUE(batch->empty());
    EXPECT_EQ(cur.Position(), pos);
    EXPECT_GE(elapsed_ms, 20);   // 确实等待（唤醒空转不允许）
    EXPECT_LT(elapsed_ms, 2000);  // 预算内返回
}

/// 溢出判据：游标落后于环回收窗口 → kCursorOverflow；ResetTo 后恢复增量。
TEST_F(SubscribeTest, CursorOverflowAndResync) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    TableDef def = MakeDef("t");
    def.row_cap = 2048;  // 三批共 1800 行
    auto t = eng->CreateTable(def);
    ASSERT_TRUE(t.IsOk());
    const TableHandle table = t.Value();

    auto sub = eng->Subscribe({"t"});
    ASSERT_TRUE(sub.IsOk());
    ChangeCursor cur = sub.TakeValue();

    // 三批 × 600 行：总量远超 64KB 环容量，发生覆盖回收。
    int64_t next_id = 1;
    for (int batch_no = 0; batch_no < 3; ++batch_no) {
        std::vector<Engine::BatchOp> ops;
        for (int i = 0; i < 600; ++i) {
            Engine::BatchOp op;
            op.type = Engine::BatchOp::Type::kInsert;
            op.table = table;
            op.row = Row(next_id, next_id * 10);
            ++next_id;
            ops.push_back(std::move(op));
        }
        const auto rb = eng->ExecuteBatch(ops);
        ASSERT_TRUE(rb.IsOk()) << "batch " << batch_no << ": " << rtdb::ErrorMessage(rb.Error());
    }

    auto bad = cur.Next(0);
    ASSERT_FALSE(bad.IsOk());
    EXPECT_EQ(bad.Error(), Err::kCursorOverflow);

    // 重同步：全量重取后 ResetTo(快照 seq) → 恢复空批与常规增量。
    cur.ResetTo(static_cast<uint64_t>(eng->CommittedSeq()));
    auto ok = cur.Next(0);
    ASSERT_TRUE(ok.IsOk()) << rtdb::ErrorMessage(ok.Error());
    EXPECT_TRUE(ok->empty());

    ASSERT_TRUE(eng->Insert(table, Row(9001, 7)).IsOk());
    auto after = cur.Next(0);
    ASSERT_TRUE(after.IsOk());
    ASSERT_EQ(after->events.size(), 1u);
    EXPECT_EQ(after->events[0].kind, ChangeEvent::Kind::kInsert);
    EXPECT_EQ(after->events[0].row[0].as_int(), 9001);
}

/// P0-5：冷快照只推进 checkpoint，不主动清空 WAL 环——活跃订阅游标
/// 不会仅因抓快照而溢出，仍可继续增量读取。
TEST_F(SubscribeTest, SnapshotDoesNotEvictActiveCursor) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    auto t = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t.IsOk());
    const TableHandle table = t.Value();

    auto sub = eng->Subscribe({"t"});
    ASSERT_TRUE(sub.IsOk());
    ChangeCursor cur = sub.TakeValue();
    const uint64_t base = cur.Position();

    for (int64_t i = 1; i <= 100; ++i)
        ASSERT_TRUE(eng->Insert(table, Row(i, i * 10)).IsOk());

    auto snap = eng->CreateSnapshot();
    ASSERT_TRUE(snap.IsOk()) << rtdb::ErrorMessage(snap.Error());

    // 旧行为（DropAllRetained）会在此返回 kCursorOverflow；新保留策略下
    // tail 仍在游标之后，100 条变更完整交付。
    auto batch = cur.Next(0);
    ASSERT_TRUE(batch.IsOk()) << rtdb::ErrorMessage(batch.Error());
    ASSERT_EQ(batch->events.size(), 100u);
    EXPECT_EQ(batch->events.front().seq, base + 1);
    EXPECT_EQ(batch->events.back().row[0].as_int(), 100);

    std::error_code ec;
    std::filesystem::remove(snap.Value(), ec);
}

/// P0-5：环写满时的回收只推进“刚好够写”的水位，保留最新后缀——落后
/// 但仍在保留窗口内的订阅游标可继续增量读取，不必整表重取。
TEST_F(SubscribeTest, RingPressureKeepsRecentWindowForLaggingCursor) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk()) << rtdb::ErrorMessage(open.Error());
    auto eng = open.TakeValue();
    TableDef def = MakeDef("t");
    def.row_cap = 2048;
    auto t = eng->CreateTable(def);
    ASSERT_TRUE(t.IsOk());
    const TableHandle table = t.Value();

    auto sub = eng->Subscribe({"t"});
    ASSERT_TRUE(sub.IsOk());
    ChangeCursor cur = sub.TakeValue();

    auto run_batch = [&](int64_t first, int n) {
        std::vector<Engine::BatchOp> ops;
        ops.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) {
            Engine::BatchOp op;
            op.type = Engine::BatchOp::Type::kInsert;
            op.table = table;
            op.row = Row(first + i, (first + i) * 10);
            ops.push_back(std::move(op));
        }
        const auto rb = eng->ExecuteBatch(ops);
        ASSERT_TRUE(rb.IsOk()) << rtdb::ErrorMessage(rb.Error());
    };

    // 先写 100 行并消费，游标停在“已交付 100 行”的位置。
    run_batch(1, 100);
    auto b1 = cur.Next(0);
    ASSERT_TRUE(b1.IsOk()) << rtdb::ErrorMessage(b1.Error());
    ASSERT_EQ(b1->events.size(), 100u);
    const uint64_t pos_after_first = cur.Position();

    // 再写 600 行（环接近满），游标保持滞后；随后 100 行批强制触发
    // 覆盖回收。旧 DropAllRetained 会把 tail 推到 head，使 pos_after_first
    // 落入已回收区；新策略只丢最老一小段，游标仍在窗口内。
    run_batch(101, 600);
    run_batch(701, 100);

    auto b2 = cur.Next(0);
    ASSERT_TRUE(b2.IsOk()) << rtdb::ErrorMessage(b2.Error());
    ASSERT_EQ(b2->events.size(), 700u);  // 101..800，无缺漏
    EXPECT_EQ(b2->events.front().row[0].as_int(), 101);
    EXPECT_EQ(b2->events.back().row[0].as_int(), 800);
    EXPECT_GT(cur.Position(), pos_after_first);
}

/// 并发写：跨线程写入经平台唤醒逐条交付 —— 顺序、无丢失、无重复。
TEST_F(SubscribeTest, ConcurrentWriterNoLossNoDuplicate) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t.IsOk());
    const TableHandle table = t.Value();

    auto sub = eng->Subscribe({"t"});
    ASSERT_TRUE(sub.IsOk());
    ChangeCursor cur = sub.TakeValue();

    constexpr int64_t kRows = 200;
    std::atomic<bool> writer_ok{true};
    std::thread writer([&] {
        for (int64_t i = 1; i <= kRows; ++i) {
            if (!eng->Insert(table, Row(i, i)).IsOk()) {
                writer_ok.store(false);
                return;
            }
        }
    });

    std::vector<int64_t> ids;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (static_cast<int64_t>(ids.size()) < kRows &&
           std::chrono::steady_clock::now() < deadline) {
        auto batch = cur.Next(50);
        ASSERT_TRUE(batch.IsOk()) << rtdb::ErrorMessage(batch.Error());
        for (const ChangeEvent& e : batch->events) {
            EXPECT_EQ(e.kind, ChangeEvent::Kind::kInsert);
            ASSERT_FALSE(e.row.empty());
            ids.push_back(e.row[0].as_int());
        }
    }
    writer.join();
    EXPECT_TRUE(writer_ok.load());
    ASSERT_EQ(static_cast<int64_t>(ids.size()), kRows);
    for (int64_t i = 0; i < kRows; ++i) EXPECT_EQ(ids[static_cast<size_t>(i)], i + 1);
}

/// 全部表模式（空过滤器）：表名经目录解析进入事件。
TEST_F(SubscribeTest, AllTablesFilterResolvesName) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t.IsOk());

    auto sub = eng->Subscribe({});
    ASSERT_TRUE(sub.IsOk()) << rtdb::ErrorMessage(sub.Error());
    ChangeCursor cur = sub.TakeValue();

    ASSERT_TRUE(eng->Insert(t.Value(), Row(1, 10)).IsOk());
    auto batch = cur.Next(0);
    ASSERT_TRUE(batch.IsOk());
    ASSERT_EQ(batch->events.size(), 1u);
    EXPECT_EQ(batch->events[0].table, "t");
}

/// 扩展列：事件行 = 内联列 + 该次写入携带的 ext 列（未携带列不产出）。
TEST_F(SubscribeTest, ExtColumnIncludedInEvents) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t_res = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t_res.IsOk());
    const TableHandle old_handle = t_res.Value();  // 2 列视图
    ASSERT_TRUE(eng->Insert(old_handle, Row(1, 10)).IsOk());
    ASSERT_TRUE(eng->AlterTableAddColumn(old_handle, ExtCol("tag", DataType::kInt64,
                                                            /*nullable=*/true, /*has_default=*/true,
                                                            /*def_int=*/42))
                    .IsOk());
    auto t2_res = eng->OpenTable("t");
    ASSERT_TRUE(t2_res.IsOk());
    const TableHandle new_handle = t2_res.Value();  // 3 列视图

    auto sub = eng->Subscribe({"t"});
    ASSERT_TRUE(sub.IsOk());
    ChangeCursor cur = sub.TakeValue();

    // 新句柄写入带 ext 值：事件行 3 列。
    ASSERT_TRUE(eng->Insert(new_handle,
                            {ColumnValue::Int(2), ColumnValue::Int(20), ColumnValue::Int(7)})
                    .IsOk());
    auto b1 = cur.Next(0);
    ASSERT_TRUE(b1.IsOk());
    ASSERT_EQ(b1->events.size(), 1u);
    ASSERT_EQ(b1->events[0].row.size(), 3u);
    EXPECT_EQ(b1->events[0].row[2].as_int(), 7);

    // 旧句柄写入（无 ext 后缀）：事件行 2 列（默认值语义由消费方补充）。
    ASSERT_TRUE(eng->Insert(old_handle, Row(3, 30)).IsOk());
    auto b2 = cur.Next(0);
    ASSERT_TRUE(b2.IsOk());
    ASSERT_EQ(b2->events.size(), 1u);
    EXPECT_EQ(b2->events[0].row.size(), 2u);

    // 显式 NULL ext：事件行携带 NULL。
    ASSERT_TRUE(eng->UpdateByKey(new_handle, ColumnValue::Int(2),
                                 {ColumnValue::Int(2), ColumnValue::Int(21), ColumnValue::Null()})
                    .IsOk());
    auto b3 = cur.Next(0);
    ASSERT_TRUE(b3.IsOk());
    ASSERT_EQ(b3->events.size(), 1u);
    EXPECT_EQ(b3->events[0].kind, ChangeEvent::Kind::kUpdate);
    ASSERT_EQ(b3->events[0].row.size(), 3u);
    EXPECT_TRUE(b3->events[0].row[2].is_null());
}

/// 变长列语义：仅当事件仍为行当前版本时解码真实值；被覆盖写/删除的事件
/// 交付 NULL（池块可能已回收，防交付串行字节，docs/13 §3-W2）。
TEST_F(SubscribeTest, VarColumnCurrentVersionSemantics) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    TableDef def;
    def.name = "t";
    def.columns.push_back({"id", DataType::kInt64, 0, false, false});
    def.columns.push_back({"tag", DataType::kUtf8Var, 0, true, false});
    auto t = eng->CreateTable(def);
    ASSERT_TRUE(t.IsOk());
    const TableHandle table = t.Value();

    auto sub = eng->Subscribe({"t"});
    ASSERT_TRUE(sub.IsOk());
    ChangeCursor cur = sub.TakeValue();

    // 当前版本：真实值。
    ASSERT_TRUE(eng->Insert(table, {ColumnValue::Int(1), ColumnValue::Text("a")}).IsOk());
    auto b1 = cur.Next(0);
    ASSERT_TRUE(b1.IsOk());
    ASSERT_EQ(b1->events.size(), 1u);
    ASSERT_EQ(b1->events[0].row.size(), 2u);
    EXPECT_EQ(b1->events[0].row[1].as_bytes(), "a");

    // 同窗内先插入后被覆盖：插入事件变长列 NULL，覆盖事件交付真实值。
    ASSERT_TRUE(eng->Insert(table, {ColumnValue::Int(2), ColumnValue::Text("b")}).IsOk());
    ASSERT_TRUE(eng->UpdateByKey(table, ColumnValue::Int(2),
                                 {ColumnValue::Int(2), ColumnValue::Text("b2")})
                    .IsOk());
    auto b2 = cur.Next(0);
    ASSERT_TRUE(b2.IsOk());
    ASSERT_EQ(b2->events.size(), 2u);
    EXPECT_EQ(b2->events[0].kind, ChangeEvent::Kind::kInsert);
    EXPECT_TRUE(b2->events[0].row[1].is_null());
    EXPECT_EQ(b2->events[1].kind, ChangeEvent::Kind::kUpdate);
    EXPECT_EQ(b2->events[1].row[1].as_bytes(), "b2");

    // 删除：此前的写入事件（已墓碑）变长列 NULL；delete 事件交付主键。
    ASSERT_TRUE(eng->Insert(table, {ColumnValue::Int(3), ColumnValue::Text("c")}).IsOk());
    ASSERT_TRUE(eng->DeleteByKey(table, ColumnValue::Int(3)).IsOk());
    auto b3 = cur.Next(0);
    ASSERT_TRUE(b3.IsOk());
    ASSERT_EQ(b3->events.size(), 2u);
    EXPECT_EQ(b3->events[0].kind, ChangeEvent::Kind::kInsert);
    EXPECT_TRUE(b3->events[0].row[1].is_null());
    EXPECT_EQ(b3->events[1].kind, ChangeEvent::Kind::kDelete);
    EXPECT_EQ(b3->events[1].key.as_int(), 3);
}

/// 拒绝面：订阅不存在的表 → kTableNotFound。
TEST_F(SubscribeTest, TableNotFoundRejected) {
    auto open = Engine::Open(Opts());
    ASSERT_TRUE(open.IsOk());
    auto eng = open.TakeValue();
    auto t = eng->CreateTable(MakeDef("t"));
    ASSERT_TRUE(t.IsOk());

    auto sub = eng->Subscribe({"nope"});
    ASSERT_FALSE(sub.IsOk());
    EXPECT_EQ(sub.Error(), Err::kTableNotFound);
}

/// 拒绝面：Reader 主角色未挂载 WAL 环 → kUnsupported（写侧可订阅）。
TEST_F(SubscribeTest, ReaderRoleUnsupported) {
    {
        auto open = Engine::Open(Opts());
        ASSERT_TRUE(open.IsOk());
        auto eng = open.TakeValue();
        auto t = eng->CreateTable(MakeDef("t"));
        ASSERT_TRUE(t.IsOk());
        auto sub = eng->Subscribe({});
        ASSERT_TRUE(sub.IsOk()) << rtdb::ErrorMessage(sub.Error());
        ChangeCursor cur = sub.TakeValue();
        ASSERT_TRUE(eng->Insert(t.Value(), Row(1, 10)).IsOk());
        auto batch = cur.Next(0);
        ASSERT_TRUE(batch.IsOk());
        EXPECT_EQ(batch->events.size(), 1u);
    }
    rtdb::Options ropts = Opts(rtdb::Role::Reader);
    ropts.instance = "w2_sub_reader";
    auto ropen = Engine::Open(ropts);
    ASSERT_TRUE(ropen.IsOk()) << rtdb::ErrorMessage(ropen.Error());
    auto r = ropen.TakeValue();
    ASSERT_TRUE(r->OpenTable("t").IsOk());
    auto sub = r->Subscribe({"t"});
    ASSERT_FALSE(sub.IsOk());
    EXPECT_EQ(sub.Error(), Err::kUnsupported);
}

}  // namespace
