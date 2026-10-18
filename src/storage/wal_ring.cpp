#include "storage/wal_ring.hpp"

#include <chrono>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "infra/crc32.hpp"
#include "infra/failpoint.hpp"
#include "infra/shm_region.hpp"  // layout::RefreshLayoutCrc（稳定字段 CRC 刷新）
#include "storage/catalog.hpp"
#include "storage/table_store.hpp"

namespace rtdb {
namespace storage {

using infra::Crc32;
using infra::Crc32Update;

namespace {

int64_t NowUnixNs() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

}  // namespace

// ============================================================
// WalRing
// ============================================================

uint64_t WalRing::RecordBytes(uint32_t payload_len) noexcept {
    return static_cast<uint64_t>(kWalRecHdrBytes) + payload_len + kWalRecCrcBytes;
}

uint8_t* WalRing::Area() const noexcept { return area_; }

uint64_t WalRing::PendingBytes() const noexcept {
    if (hdr_->head_seq == hdr_->tail_seq) return 0;  // 空环
    if (hdr_->head_pos > hdr_->tail_pos) return hdr_->head_pos - hdr_->tail_pos;
    return area_bytes_ - hdr_->tail_pos + hdr_->head_pos;
}

bool WalRing::HasRoomFor(uint64_t rec_bytes) const noexcept {
    if (area_bytes_ < rec_bytes) return false;
    const uint64_t pending = PendingBytes();
    if (pending == 0) return true;  // 空环：任意 ≤ 区容量的记录可写（必要时垫片回绕）
    if (area_bytes_ - pending < rec_bytes) return false;  // 总量不足
    // 回绕落位约束（P0-3）：[head_pos, area) 不足时记录将从 0 起写，须容身
    // [0, tail_pos)——否则垫片回绕后覆盖 [tail_pos, head_pos) 的既有数据。
    if (area_bytes_ - hdr_->head_pos < rec_bytes && rec_bytes > hdr_->tail_pos) return false;
    return true;
}

Result<std::unique_ptr<WalRing>> WalRing::CreateOrAttach(void* base_addr,
                                                         infra::SlabShmAllocator* alloc,
                                                         layout::shm_SuperBlock* sb,
                                                         bool* out_created) {
    if (out_created != nullptr) *out_created = false;
    if (base_addr == nullptr || sb == nullptr)
        return Result<std::unique_ptr<WalRing>>::Fail(Err::kInvalidArgument);

    auto ring = std::unique_ptr<WalRing>(new WalRing());

    if (sb->wal_ring_off == 0) {
        // 新建：单块 Slab 分配（默认 64KB = slab 最大级），整块清零。
        if (alloc == nullptr) return Result<std::unique_ptr<WalRing>>::Fail(Err::kInvalidArgument);
        Result<uint64_t> r = alloc->Allocate(kDefaultWalRingBytes);
        if (!r.IsOk()) return Result<std::unique_ptr<WalRing>>::Fail(r.Error());
        const uint64_t off = r.Value();
        auto* block = static_cast<uint8_t*>(alloc->OffsetToPtr(off));
        std::memset(block, 0, static_cast<size_t>(kDefaultWalRingBytes));
        auto* hdr = reinterpret_cast<shm_WalRingHeader*>(block);
        hdr->magic = kWalRingMagic;
        hdr->area_bytes = kDefaultWalRingBytes - sizeof(shm_WalRingHeader);
        hdr->head_seq = 1;
        hdr->tail_seq = 1;
        hdr->head_pos = 0;
        hdr->tail_pos = 0;
        // 与运行期一致走原子视角（此处虽为单线程建环，统一访问方式即可）。
        AsAtomicU32(&hdr->poisoned)->store(0);
        sb->wal_ring_off = off;
        sb->wal_ring_bytes = kDefaultWalRingBytes;
        layout::RefreshLayoutCrc(sb);
        ring->hdr_ = hdr;
        ring->area_ = block + sizeof(shm_WalRingHeader);
        ring->area_bytes_ = hdr->area_bytes;
        if (out_created != nullptr) *out_created = true;
        return Result<std::unique_ptr<WalRing>>::Ok(std::move(ring));
    }

    // 附着既有环。
    if (sb->wal_ring_bytes < sizeof(shm_WalRingHeader) + kMinWalAreaBytes)
        return Result<std::unique_ptr<WalRing>>::Fail(Err::kCorruption);
    auto* hdr =
        reinterpret_cast<shm_WalRingHeader*>(static_cast<char*>(base_addr) + sb->wal_ring_off);
    if (hdr->magic != kWalRingMagic) return Result<std::unique_ptr<WalRing>>::Fail(Err::kCorruption);
    if (hdr->area_bytes != sb->wal_ring_bytes - sizeof(shm_WalRingHeader))
        return Result<std::unique_ptr<WalRing>>::Fail(Err::kCorruption);
    if (hdr->head_seq < hdr->tail_seq) return Result<std::unique_ptr<WalRing>>::Fail(Err::kCorruption);
    if (hdr->head_pos >= hdr->area_bytes || hdr->tail_pos >= hdr->area_bytes)
        return Result<std::unique_ptr<WalRing>>::Fail(Err::kCorruption);
    // 覆盖协议不变量：(checkpoint_seq, committed_seq] 重放区间必须完整在环内，
    // 故环内最老保留 seq 不得越过 checkpoint + 1。checkpoint_seq 由引擎/快照/
    // 组提交以原子视角写（AsAtomicU64），这里必须原子读，避免混用访问竞争。
    const uint64_t ckpt = layout::AsAtomicU64(&sb->checkpoint_seq)->load();
    if (hdr->tail_seq > ckpt + 1)
        return Result<std::unique_ptr<WalRing>>::Fail(Err::kCorruption);

    ring->hdr_ = hdr;
    ring->area_ = reinterpret_cast<uint8_t*>(hdr) + sizeof(shm_WalRingHeader);
    ring->area_bytes_ = hdr->area_bytes;
    return Result<std::unique_ptr<WalRing>>::Ok(std::move(ring));
}

Err WalRing::WriteTailPadAndWrap() noexcept {
    const uint64_t remaining = area_bytes_ - hdr_->head_pos;
    if (remaining >= kWalMinRecordBytes) {
        uint8_t hdr_buf[kWalRecHdrBytes];
        std::memset(hdr_buf, 0, sizeof(hdr_buf));
        EncodeU32LE(hdr_buf + 0, static_cast<uint32_t>(remaining));
        EncodeU32LE(hdr_buf + 4, static_cast<uint32_t>(remaining - kWalMinRecordBytes));
        hdr_buf[48] = kWalOpPad;
        uint8_t* dst = area_ + hdr_->head_pos;
        std::memcpy(dst, hdr_buf, kWalRecHdrBytes);
        // 载荷区清零（确定性内容，CRC 可复算）。
        std::memset(dst + kWalRecHdrBytes, 0,
                    static_cast<size_t>(remaining - kWalMinRecordBytes - kWalRecCrcBytes));
        const uint32_t crc = Crc32(dst, static_cast<size_t>(remaining) - kWalRecCrcBytes);
        EncodeU32LE(dst + remaining - kWalRecCrcBytes, crc);
    }
    hdr_->head_pos = 0;
    return Err::kOk;
}

Err WalRing::Append(uint8_t op, int64_t table_id, uint64_t row_no, uint64_t row_ver,
                    const uint8_t* payload, uint32_t payload_len, uint64_t* out_seq) noexcept {
    if (hdr_ == nullptr) return Err::kUnsupported;
    if (hdr_->magic != kWalRingMagic) return Err::kCorruption;
    if (AsAtomicU32(&hdr_->poisoned)->load() != 0) return Err::kPoisoned;

    const uint64_t consumed = RecordBytes(payload_len);
    // 空间判据与 HasRoomFor 保持单一事实源（含垫片回绕落位约束，P0-3）。
    if (!HasRoomFor(consumed)) return Err::kLimitExceeded;  // 调用方未确认空间

    if (area_bytes_ - hdr_->head_pos < consumed) {
        const Err pe = WriteTailPadAndWrap();
        if (!IsOk(pe)) return pe;
    }

    const uint64_t seq = hdr_->head_seq;
    uint8_t hdr_buf[kWalRecHdrBytes];
    std::memset(hdr_buf, 0, sizeof(hdr_buf));
    EncodeU32LE(hdr_buf + 0, static_cast<uint32_t>(consumed));
    EncodeU32LE(hdr_buf + 4, payload_len);
    EncodeU64LE(hdr_buf + 8, seq);
    EncodeU64LE(hdr_buf + 16, static_cast<uint64_t>(NowUnixNs()));
    EncodeU64LE(hdr_buf + 24, row_no);
    EncodeU64LE(hdr_buf + 32, row_ver);
    EncodeU64LE(hdr_buf + 40, static_cast<uint64_t>(table_id));
    hdr_buf[48] = op;

    uint8_t* dst = area_ + hdr_->head_pos;
    std::memcpy(dst, hdr_buf, kWalRecHdrBytes);
    // 注入点：头部已写、载荷未写 → 撕裂记录（崩溃注入用例验证扫描终止语义）。
    failpoint::CrashPoint("wal.append.mid");
    if (payload_len != 0 && payload != nullptr)
        std::memcpy(dst + kWalRecHdrBytes, payload, payload_len);
    // CRC 分段续算（头部 + 载荷），随后写入尾部。
    const uint32_t crc = Crc32Update(Crc32Update(0, dst, kWalRecHdrBytes),
                                     dst + kWalRecHdrBytes, payload_len);
    EncodeU32LE(dst + kWalRecHdrBytes + payload_len, crc);

    hdr_->head_seq = seq + 1;
    hdr_->head_pos += consumed;
    if (hdr_->head_pos >= area_bytes_) hdr_->head_pos = 0;
    if (out_seq != nullptr) *out_seq = seq;
    return Err::kOk;
}

Err WalRing::AppendMark(uint8_t op, const uint8_t* payload, uint32_t payload_len,
                        uint64_t* out_seq) noexcept {
    return Append(op, 0, 0, 0, payload, payload_len, out_seq);
}

void WalRing::DropAllRetained() noexcept {
    hdr_->tail_pos = hdr_->head_pos;
    hdr_->tail_seq = hdr_->head_seq;
}

// 恢复期写头回绕（单线程恢复路径专用，见头文件文档）。
// 语义：把 head 退回“最后一条有效/已提交记录之后”，使下一次 Append 的 seq
// 紧接有效前缀（seq 连续），从而 committed_seq 永不跨过从未 COMMIT 的区间。
//
// 边界说明：
//  - 回退可使环逻辑为空（next_seq == tail_seq）：PendingBytes() 以
//    head_seq == tail_seq 判空（返回 0），HasRoomFor() 随后按满容量放行，
//    位置字段（head_pos/tail_pos 可能不等）在该判空语义下不参与计算；
//  - 回退后 head_pos 为“有效前缀末记录的归一化结束位置”，若恰为区尾则
//    已归一化为 0，与 Append 写路径的 head_pos 归一化一致（可继续回绕写）；
//  - 回退只允许在当前保留窗口内（tail_seq ≤ next_seq ≤ head_seq），越界
//    视为恢复期自洽性破坏（kCorruption），绝不静默截断/前移。
Err WalRing::RewindHeadAfterReplay(uint64_t next_seq, uint64_t next_pos) noexcept {
    if (hdr_ == nullptr) return Err::kUnsupported;
    if (hdr_->magic != kWalRingMagic) return Err::kCorruption;
    // 只允许在当前保留窗口内回退（不得越过 tail，也不得前移越过 head）。
    if (next_seq < hdr_->tail_seq || next_seq > hdr_->head_seq) return Err::kCorruption;
    if (next_pos >= area_bytes_) return Err::kCorruption;
    hdr_->head_seq = next_seq;
    hdr_->head_pos = next_pos;
    return Err::kOk;
}

void WalRing::RewindHeadToTail() noexcept {
    if (hdr_ == nullptr) return;
    hdr_->head_seq = hdr_->tail_seq;
    hdr_->head_pos = hdr_->tail_pos;
}

Err WalRing::ScanValid(const RecordFn& fn, uint64_t* out_last_seq) const {
    if (out_last_seq != nullptr) *out_last_seq = 0;
    if (hdr_ == nullptr || hdr_->magic != kWalRingMagic) return Err::kCorruption;
    if (hdr_->head_seq == hdr_->tail_seq) return Err::kOk;  // 空环
    if (hdr_->head_seq < hdr_->tail_seq) return Err::kCorruption;

    uint64_t pos = hdr_->tail_pos;
    const uint64_t head_pos = hdr_->head_pos;
    uint64_t expect_seq = hdr_->tail_seq;
    uint64_t last = 0;
    // 步数上界：每条记录 ≥ kWalMinRecordBytes；回绕垫片与裸回绕额外留余量。
    const uint64_t max_steps = area_bytes_ / kWalMinRecordBytes + 16;
    uint64_t steps = 0;
    bool first = true;  // 满环时 tail_pos == head_pos，仍须扫整环一圈

    while (first || pos != head_pos) {
        first = false;
        if (++steps > max_steps) return Err::kCorruption;

        const uint64_t remaining = area_bytes_ - pos;
        if (remaining < kWalMinRecordBytes) {  // 裸回绕垫片（写方保证此处无记录起始）
            pos = 0;
            continue;
        }

        const uint32_t total_len = DecodeU32LE(area_ + pos);
        if (total_len < kWalMinRecordBytes || pos + total_len > area_bytes_) break;  // 撕裂尾部

        const uint8_t op = area_[pos + 48];
        const uint32_t crc_stored = DecodeU32LE(area_ + pos + total_len - kWalRecCrcBytes);
        const uint32_t crc_calc = Crc32(area_ + pos, total_len - kWalRecCrcBytes);
        if (crc_stored != crc_calc) {
            // 撕裂 pad 抢救：pad 从不重放，total_len 完好即可跳过（回绕后记录可达）。
            if (op == kWalOpPad) {
                pos += total_len;
                if (pos >= area_bytes_) pos = 0;
                continue;
            }
            break;  // 撕裂尾部：有效数据终点（kill -9 / 断电的正常痕迹）
        }

        if (op == kWalOpPad) {
            pos += total_len;
            if (pos >= area_bytes_) pos = 0;
            continue;
        }

        const uint64_t seq = DecodeU64LE(area_ + pos + 8);
        if (seq != expect_seq) return Err::kCorruption;  // 确认前缀内不允许 seq 断档
        const uint32_t payload_len = DecodeU32LE(area_ + pos + 4);
        if (kWalRecHdrBytes + static_cast<uint64_t>(payload_len) + kWalRecCrcBytes > total_len)
            return Err::kCorruption;

        WalRecordView view;
        view.seq = seq;
        view.ts_ns = DecodeU64LE(area_ + pos + 16);
        view.row_no = DecodeU64LE(area_ + pos + 24);
        view.row_ver = DecodeU64LE(area_ + pos + 32);
        view.table_id = static_cast<int64_t>(DecodeU64LE(area_ + pos + 40));
        view.op = op;
        view.payload_len = payload_len;
        view.payload = payload_len != 0 ? area_ + pos + kWalRecHdrBytes : nullptr;
        // 归一化结束位置（== area_bytes 归一化为 0，与 hdr.head_pos 同语义；
        // 上面的 pos + total_len > area_bytes_ 已排除越界）。
        view.end_pos = (pos + total_len >= area_bytes_) ? 0 : (pos + total_len);

        if (fn != nullptr) {
            const Err fe = fn(view);
            if (!IsOk(fe)) return fe;
        }
        last = seq;
        expect_seq = seq + 1;
        pos += total_len;
        if (pos >= area_bytes_) pos = 0;
    }

    if (out_last_seq != nullptr) *out_last_seq = last;
    return Err::kOk;
}

Err WalRing::ScanRange(uint64_t from_seq_excl, uint64_t to_seq_incl, const ScanRangeFn& fn,
                       uint64_t* out_last_seq) const {
    if (out_last_seq != nullptr) *out_last_seq = from_seq_excl;
    if (hdr_ == nullptr || hdr_->magic != kWalRingMagic) return Err::kCorruption;
    if (to_seq_incl <= from_seq_excl) return Err::kOk;  // 空区间：无数据需求
    if (from_seq_excl + 1 < hdr_->tail_seq) return Err::kCursorOverflow;
    if (hdr_->head_seq < hdr_->tail_seq) return Err::kCorruption;
    if (hdr_->head_seq == hdr_->tail_seq) return Err::kOk;       // 空环
    if (from_seq_excl + 1 >= hdr_->head_seq) return Err::kOk;    // 起点越过已写入上限

    uint64_t pos = hdr_->tail_pos;
    const uint64_t head_pos = hdr_->head_pos;
    uint64_t expect_seq = hdr_->tail_seq;
    uint64_t last = from_seq_excl;
    // 步数上界：每条记录 ≥ kWalMinRecordBytes；回绕垫片与裸回绕额外留余量。
    const uint64_t max_steps = area_bytes_ / kWalMinRecordBytes + 16;
    uint64_t steps = 0;
    bool first = true;  // 满环时 tail_pos == head_pos，仍须扫整环一圈

    while (first || pos != head_pos) {
        first = false;
        if (++steps > max_steps) return Err::kCorruption;

        const uint64_t remaining = area_bytes_ - pos;
        if (remaining < kWalMinRecordBytes) {  // 裸回绕垫片
            pos = 0;
            continue;
        }

        const uint32_t total_len = DecodeU32LE(area_ + pos);
        if (total_len < kWalMinRecordBytes || pos + total_len > area_bytes_) break;  // 撕裂尾部

        const uint8_t op = area_[pos + 48];
        const uint32_t crc_stored = DecodeU32LE(area_ + pos + total_len - kWalRecCrcBytes);
        const uint32_t crc_calc = Crc32(area_ + pos, total_len - kWalRecCrcBytes);
        if (crc_stored != crc_calc) {
            // 撕裂 pad 抢救：pad 从不重放，total_len 完好即可跳过。
            if (op == kWalOpPad) {
                pos += total_len;
                if (pos >= area_bytes_) pos = 0;
                continue;
            }
            break;  // 撕裂尾部：有效数据终点
        }

        if (op == kWalOpPad) {
            pos += total_len;
            if (pos >= area_bytes_) pos = 0;
            continue;
        }

        const uint64_t seq = DecodeU64LE(area_ + pos + 8);
        if (seq != expect_seq) return Err::kCorruption;  // 环内不允许 seq 断档
        const uint32_t payload_len = DecodeU32LE(area_ + pos + 4);
        if (kWalRecHdrBytes + static_cast<uint64_t>(payload_len) + kWalRecCrcBytes > total_len)
            return Err::kCorruption;

        if (seq > to_seq_incl) break;  // 越过区间上界：正常结束

        if (seq > from_seq_excl) {
            WalRecordView view;
            view.seq = seq;
            view.ts_ns = DecodeU64LE(area_ + pos + 16);
            view.row_no = DecodeU64LE(area_ + pos + 24);
            view.row_ver = DecodeU64LE(area_ + pos + 32);
            view.table_id = static_cast<int64_t>(DecodeU64LE(area_ + pos + 40));
            view.op = op;
            view.payload_len = payload_len;
            view.payload = payload_len != 0 ? area_ + pos + kWalRecHdrBytes : nullptr;
            // 归一化结束位置（同 ScanValid 语义）。
            view.end_pos = (pos + total_len >= area_bytes_) ? 0 : (pos + total_len);
            const bool keep = (fn == nullptr) ? true : fn(view);
            if (!keep) break;  // 当前记录未交付：游标停在它之前
            last = seq;
        }
        expect_seq = seq + 1;
        pos += total_len;
        if (pos >= area_bytes_) pos = 0;
    }

    if (out_last_seq != nullptr) *out_last_seq = last;
    return Err::kOk;
}

// ============================================================
// 恢复重放（docs/08 §3-W4 恢复路径 v1）
// ============================================================

Err ReplayWal(Catalog* catalog, void* base_addr, infra::SlabShmAllocator* alloc, WalRing* ring,
              layout::shm_SuperBlock* sb, ReplayStats* out_stats) {
    if (catalog == nullptr || base_addr == nullptr || ring == nullptr || sb == nullptr)
        return Err::kInvalidArgument;

    ReplayStats st;
    // checkpoint_seq 由引擎/快照/组提交以原子视角写（AsAtomicU64），重放读取
    // 亦须原子，避免与后台 flush 线程混用访问方式（TSan 数据竞争）。
    const uint64_t ckpt = layout::AsAtomicU64(&sb->checkpoint_seq)->load();

    // 重放触及的表存储缓存（table_id → store；按需附着）。
    std::unordered_map<uint64_t, TableStore*> touched;
    std::unordered_map<uint64_t, std::unique_ptr<TableStore>> stores;

    auto store_for = [&](uint64_t table_id, Err* err) -> TableStore* {
        auto t = touched.find(table_id);
        if (t != touched.end()) return t->second;
        const Result<uint64_t> desc_res = catalog->FindById(table_id);
        if (!desc_res.IsOk()) return nullptr;  // 表不存在（建表 DDL 未持久化）→ 记录跳过
        const uint64_t desc_off = desc_res.Value();
        auto sit = stores.find(desc_off);
        if (sit == stores.end()) {
            auto att = TableStore::Attach(base_addr, alloc, desc_off);
            if (!att.IsOk()) {
                *err = att.Error();
                return nullptr;
            }
            sit = stores.emplace(desc_off, att.TakeValue()).first;
        }
        TableStore* s = sit->second.get();
        touched.emplace(table_id, s);
        return s;
    };

    // 单条记录的幂等重放（seq ≤ checkpoint 的记录跳过——已随 checkpoint 落盘应用）。
    auto apply_one = [&](const WalRecordView& r) -> Err {
        if (r.seq <= ckpt) return Err::kOk;
        st.records_applied++;
        switch (r.op) {
            case kWalOpInsert:
            case kWalOpUpdate: {
                Err err = Err::kOk;
                TableStore* s = store_for(static_cast<uint64_t>(r.table_id), &err);
                if (s == nullptr) return err;  // kOk = 跳过；否则透传附着失败
                return s->ReplayUpsert(r.row_no, r.row_ver, r.payload, r.payload_len);
            }
            case kWalOpDelete: {
                Err err = Err::kOk;
                TableStore* s = store_for(static_cast<uint64_t>(r.table_id), &err);
                if (s == nullptr) return err;
                return s->ReplayDelete(r.row_no, r.row_ver, r.payload);
            }
            case kWalOpCreateTable:
            case kWalOpDropTable:
                // M1-W4 边界：DDL 记录仅入环观测（目录事务本身按
                // "先写描述符再挂树" 顺序崩溃安全）；重放还原归 W5/M2。
                return Err::kOk;
            default:
                return Err::kOk;
        }
    };

    std::vector<WalRecordView> batch_buf;  // 未决批记录视图（指向环内，扫描期有效）
    bool in_batch = false;
    // 有效前缀（= 已完整落环且批语义已 COMMIT 的记录序列）的末记录：
    // seq + 归一化结束位置。commit/非批记录应用成功后推进。
    uint64_t valid_end = 0;                // 可发布的最后 seq
    uint64_t valid_end_pos = 0;            // 该记录之后的 head_pos 候选
    bool valid_prefix_exists = false;      // 环内是否存在任何有效前缀
    // 进入当前批时刻的有效前缀快照：扫描结束仍在批内 → 回滚到该快照。
    uint64_t valid_end_before_batch = 0;
    uint64_t pos_before_batch = 0;
    bool had_before_batch = false;

    Err scan_err = ring->ScanValid(
        [&](const WalRecordView& r) -> Err {
            st.records_walked++;
            if (r.op == kWalOpBatchBegin) {
                in_batch = true;
                batch_buf.clear();
                valid_end_before_batch = valid_end;
                pos_before_batch = valid_end_pos;
                had_before_batch = valid_prefix_exists;
                return Err::kOk;
            }
            if (r.op == kWalOpBatchCommit) {
                for (const WalRecordView& b : batch_buf) {
                    const Err e = apply_one(b);
                    if (!IsOk(e)) return e;
                }
                batch_buf.clear();
                in_batch = false;
                valid_end = r.seq;
                valid_end_pos = r.end_pos;
                valid_prefix_exists = true;
                return Err::kOk;
            }
            if (in_batch) {
                batch_buf.push_back(r);
                return Err::kOk;
            }
            const Err e = apply_one(r);
            if (!IsOk(e)) return e;
            valid_end = r.seq;
            valid_end_pos = r.end_pos;
            valid_prefix_exists = true;
            return Err::kOk;
        },
        nullptr);
    if (!IsOk(scan_err)) return scan_err;

    // 扫描结束仍处于批内（无 COMMIT）→ 整批作废（docs/08 §2-D3）。
    if (in_batch) {
        st.batches_discarded = 1;
        valid_end = valid_end_before_batch;
        valid_end_pos = pos_before_batch;
        valid_prefix_exists = had_before_batch;
    }

    // ---- 写头回绕（崩溃遗留的未提交后缀）----
    // 崩溃时 Append 可能已为未提交批分配并完整落环了若干 seq（BEGIN 与 COMMIT
    // 之间，CRC 有效），但写头 head 停在它们之后。若只发布 committed_seq 而不
    // 回退写头，下一次 Append 会拿到 valid_end + 2（跳过被丢弃的 valid_end+1），
    // 提交后 committed_seq 跨过从未 COMMIT 的 seq 区间 → ScanRange/ChangeCursor
    // 把作废批内的记录当变更事件交付（幻影变更，违 docs/13 §3-D1）。
    // 恢复唯一写者语义下把写头退到有效前缀末记录的 end_pos，恢复 seq 连续性：
    // 下一次 Append 的 seq == valid_end + 1，与 committed_seq 无缝衔接。
    //  - 有有效前缀：head := (valid_end+1, valid_end_pos)；
    //  - 无有效前缀（崩溃批是环内全部内容）：head := tail（逻辑空环）。
    if (ring->next_seq() > valid_end + 1) {
        const uint64_t head_seq_before = ring->next_seq();
        if (valid_prefix_exists) {
            const Err rew = ring->RewindHeadAfterReplay(valid_end + 1, valid_end_pos);
            if (!IsOk(rew)) return rew;  // kCorruption：恢复窗口自洽性被破坏
        } else {
            ring->RewindHeadToTail();
        }
        st.head_rewound = (ring->next_seq() != head_seq_before);
    }

    // 触及表重建提交计数账本（committed_row_count = 索引规模）。
    for (auto& kv : touched) {
        const Err e = kv.second->RebuildCommittedCountAfterReplay();
        if (!IsOk(e)) return e;
    }

    // 发布 committed_seq（重放完成，引擎进入 READY）。
    // 不变量 1：committed_seq ≥ checkpoint_seq 恒成立——快照回收环后重放
    // 区间为空（valid_end = 0 < checkpoint），此时发布 checkpoint 而非 0，
    // 否则已提交序号将回退（W5-a 冷快照重启闭环修复：环被
    // DropAllRetained 后重开，committed 曾被误发布为 0）。
    // 不变量 2（写头回绕后仍成立）：回退后 head_seq = valid_end+1（无有效
    // 前缀时 = tail_seq），恒 > publish_seq，故下一次 Append 分配的 seq 严格
    // 大于发布值，committed_seq 只前进不回退：
    //  - 有有效前缀：publish = max(valid_end, ckpt)，head = valid_end+1。实际恒有
    //    valid_end ≥ ckpt——若 tail_seq ≤ ckpt，则 seq = ckpt 的已提交记录必在
    //    保留窗口内且 CRC 有效；若 tail_seq > ckpt，则 tail_seq = ckpt+1 ≤
    //    valid_end（保留窗口内最老记录 seq）；max 仅为异常镜像的防御性写法；
    //  - 无有效前缀 → publish = ckpt，head = tail_seq = ckpt+1（若 tail_seq ≤ ckpt，
    //    则 seq = tail_seq 的已提交记录必为有效前缀，与前提矛盾）。
    const uint64_t publish_seq = (valid_end > ckpt) ? valid_end : ckpt;
    layout::AsAtomicU64(&sb->committed_seq)->store(publish_seq);
    st.recovered_to_seq = publish_seq;
    if (out_stats != nullptr) *out_stats = st;
    return Err::kOk;
}

}  // namespace storage
}  // namespace rtdb
