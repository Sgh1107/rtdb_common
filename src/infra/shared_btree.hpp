#ifndef RTDB_SRC_INFRA_SHARED_BTREE_HPP_
#define RTDB_SRC_INFRA_SHARED_BTREE_HPP_

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cstdint>
#include <string>
#include <type_traits>

#include "infra/slab_allocator.hpp"
#include "rtdb/err.hpp"
#include "rtdb/result.hpp"

namespace rtdb {
namespace infra {

/// 有序整数键编码（docs/03 §2.2）：符号位翻转后大端序列化，
/// 使 memcmp 序即数值序，可直接作定长二进制键。
inline void EncodeOrderedInt64(int64_t v, char out[8]) noexcept {
  uint64_t u = static_cast<uint64_t>(v) ^ (1ull << 63);
  for (int i = 7; i >= 0; --i) {
    out[i] = static_cast<char>(u & 0xFFu);
    u >>= 8;
  }
}

inline int64_t DecodeOrderedInt64(const char in[8]) noexcept {
  uint64_t u = 0;
  for (int i = 0; i < 8; ++i) u = (u << 8) | static_cast<uint8_t>(in[i]);
  return static_cast<int64_t>(u ^ (1ull << 63));
}

namespace detail {

inline constexpr size_t AlignUp8(size_t v) noexcept { return (v + 7) / 8 * 8; }
inline int KeyCmp(const void* a, const void* b, size_t len) noexcept {
  return std::memcmp(a, b, len);
}

}  // namespace detail

/// 共享内存 B+ 树（对标老 COFMBTree 的模板化重写，docs/03 §2.2）。
///
/// 物理布局约束：
///   - 节点 = 恰好 4096 字节，经 SlabShmAllocator 分配，全 offset 寻址
///     （跨进程零指针）；
///   - 内部节点：[头16B | child0(8B) | (键K|右孩8B) × count]
///   - 叶节点：  [头16B | (键K|值Payload) × count]，next 链支撑范围扫
///   - 键必须定长且 memcmp 有序（int 用 EncodeOrderedInt64；字符串补0）
///
/// 并发约定：变更必须在 ShmRootMutex 临界区内执行。删除为惰性策略
/// （不合并节点），页回收归 M1 重平衡/GC。
template <size_t MaxKeyLen, typename PayloadT>
class SharedBTree {
 public:
  using Payload = PayloadT;
  static_assert(MaxKeyLen >= 1 && MaxKeyLen <= 254, "键长须在 1..254");
  static_assert(std::is_trivially_copyable<Payload>::value,
                "载荷必须可平凡拷贝");
  static constexpr uint64_t kNodeBytes = 4096;

  /// 树的持久化锚点（宿主元数据区内的双字槽）。树实例自身无状态，
  /// 同一槽可跨进程重复附着。
  struct Slot {
    uint64_t root_off;  ///< 根节点偏移；0 表示空树
    uint64_t count;     ///< 元素总数
  };
  static_assert(sizeof(Slot) == 16);

  SharedBTree(void* base_addr, SlabShmAllocator* alloc,
              Slot* slot) noexcept
      : base_(base_addr), alloc_(alloc), slot_(slot) {}

  uint64_t Size() const noexcept { return slot_->count; }

  /// 调试/测试：遍历全树校验结构不变量（层级单调、节点内严格递增、
  /// 容量上界、叶链完整、跨叶有序）。
  Err Validate(uint64_t* visited_nodes) const noexcept;

  // ------------------------ 点查 ------------------------
  bool Find(const void* key, Payload* out) const noexcept {
    uint64_t off = slot_->root_off;
    if (off == 0) return false;
    while (true) {
      const NodeHeader* n = Node(off);
      if (n->leaf != 0) {
        const int pos = LowerBound(off, key, n->count);
        if (pos < n->count && KeyCmpAt(off, pos, key) == 0) {
          if (out != nullptr)
            std::memcpy(out, LeafValuePtr(off, pos), sizeof(Payload));
          return true;
        }
        return false;
      }
      // 内部分叉约定：等键归右（sep 是右子树最小键）⇒ 严格 upper。
      off = ChildAt(off, StrictUpper(off, key, n->count));
    }
  }

  /// 调试：回放一次点查的下溯轨迹（节点off/count/选中pos/分叉键），
  /// 文本追加到 *log。生产代码勿调用。
  bool DebugFindTrace(const void* key, std::string* log) const noexcept {
    auto append_num = [&](uint64_t v) {
      char tmp[24];
      std::snprintf(tmp, sizeof(tmp), "%llu",
                    static_cast<unsigned long long>(v));
      (*log) += tmp;
    };
    uint64_t off = slot_->root_off;
    if (off == 0) {
      (*log) += "empty-tree";
      return false;
    }
    while (true) {
      const NodeHeader* n = Node(off);
      const int pos =
          n->leaf != 0 ? LowerBound(off, key, n->count)
                       : StrictUpper(off, key, n->count);
      (*log) += "[node@"; append_num(off); (*log) += " cnt=";
      append_num(n->count); (*log) += " pos="; append_num(pos);
      (*log) += (n->leaf != 0 ? " LEAF" : " INT");
      if (pos < n->count) {
        (*log) += " key[pos]@"; append_num(KeyCmpAt(off, pos, key));
        (*log) += "]";
      } else {
        (*log) += " key[pos]=END]";
      }
      if (n->leaf != 0) {
        const bool hit =
            pos < n->count && KeyCmpAt(off, pos, key) == 0;
        return hit;
      }
      off = ChildAt(off, pos);
    }
  }

  uint64_t DebugNextLeaf(uint64_t leaf_off) const noexcept {
    return Node(leaf_off)->next_leaf;
  }

  // ------------------------ 插入 ------------------------
  /// 主动分裂策略：下沉路径上遇到满子节点就地先分，保证叶插入永不
  /// 溢出；根满则先升 super-root（仅一次）。
  Err Insert(const void* key, const Payload& value) noexcept {
    if (slot_->root_off == 0) {
      Result<uint64_t> r = AllocNode(/*leaf=*/1, /*level=*/0);
      if (!r.IsOk()) return r.Error();
      slot_->root_off = r.Value();
    }
    const uint64_t root = slot_->root_off;
    if (Node(root)->count >= CapOf(Node(root))) {
      // 升级根：旧根变为唯一左孩。此操作不改变树内容。
      Result<uint64_t> nr =
          AllocNode(0, static_cast<uint8_t>(Node(root)->level + 1));
      if (!nr.IsOk()) return nr.Error();
      SetChild0(nr.Value(), root);
      if (std::getenv("RTDB_BTREE_DEBUG") != nullptr) {
        uint64_t rb = 0;
        std::memcpy(&rb,
                    static_cast<const char*>(base_) + nr.Value() + kHdr, 8);
        std::printf("[upgrade] root=%llu new=%llu child0=%llu\n",
                    static_cast<unsigned long long>(root),
                    static_cast<unsigned long long>(nr.Value()),
                    static_cast<unsigned long long>(rb));
      }
      slot_->root_off = nr.Value();
    }

    uint64_t cur = slot_->root_off;
    int guard = 0;
    static const bool bt_dbg =
        std::getenv("RTDB_BTREE_DEBUG") != nullptr;  // 临时诊断开关
    while (true) {
      NodeHeader* n = MutHeader(cur);
      if (++guard > 64) {  // 防御：树高理论 ≤ ~12（255叉），越界必是 bug
        std::printf("[btree] DESCENT GUARD TRIP cur=%llu cnt=%u lvl=%u\n",
                    static_cast<unsigned long long>(cur), n->count,
                    n->level);
        return Err::kInternal;
      }
      if (bt_dbg)
        std::printf("[hop] cur=%llu lvl=%u cnt=%u\n",
                    static_cast<unsigned long long>(cur), n->level,
                    n->count);
      if (n->leaf != 0) {
        const int pos = LowerBound(cur, key, n->count);
        if (pos < n->count && KeyCmpAt(cur, pos, key) == 0)
          return Err::kAlreadyExists;
        ShiftEntriesRight(cur, pos, n->count - pos);
        std::memcpy(KeyPtrAt(cur, static_cast<size_t>(pos)), key, MaxKeyLen);
        std::memcpy(LeafValuePtr(cur, pos), &value, sizeof(Payload));
        n->count++;
        slot_->count++;
        return Err::kOk;
      }
      // 内部：等键归右 ⇒ 用严格 upper 选孩；满孩先分裂。
      const int hop = StrictUpper(cur, key, n->count);
      uint64_t child = ChildAt(cur, hop);
      if (Node(child)->count >= CapOf(Node(child))) {
        const Err se = SplitChild(cur, hop, child);
        if (!IsOk(se)) return se;
        child = ChildAt(cur, StrictUpper(cur, key, n->count));
      }
      cur = child;
    }
  }

  // ------------------------ 删除（惰性）------------------------
  /// 仅摘条目不合并：计数准确，页复用归 M1。
  Err Remove(const void* key) noexcept {
    uint64_t off = slot_->root_off;
    if (off == 0) return Err::kNotFound;
    while (true) {
      const NodeHeader* n = Node(off);
      if (n->leaf != 0) {
        const int pos = LowerBound(off, key, n->count);
        if (!(pos < n->count && KeyCmpAt(off, pos, key) == 0))
          return Err::kNotFound;
        ShiftEntriesLeft(off, pos + 1, n->count - pos - 1);
        MutHeader(off)->count--;
        slot_->count--;
        return Err::kOk;
      }
      off = ChildAt(off, StrictUpper(off, key, n->count));
    }
  }

  // ------------------------ 范围游标 ------------------------
  /// 范围扫描游标（沿叶链单向推进）。两端的开闭由入参分别控制。
  class Cursor {
   public:
    bool Valid() const noexcept { return valid_; }
    void Key(void* out) const noexcept { std::memcpy(out, key_, MaxKeyLen); }
    Payload Value() const noexcept {
      Payload v{};
      std::memcpy(&v, val_, sizeof(Payload));
      return v;
    }
    void Next() noexcept {
      if (!valid_) return;
      ++idx_;
      tree_->LoadAndCheckHi(this);
    }

   private:
    friend class SharedBTree;
    Cursor(const SharedBTree* t, const void* hi, bool hi_incl) noexcept
        : tree_(t), hi_incl_(hi_incl), valid_(false), leaf_(0), idx_(0),
          val_(nullptr) {
      std::memcpy(hi_, hi, MaxKeyLen);
    }
    const SharedBTree* tree_;
    char hi_[MaxKeyLen];
    bool hi_incl_;
    bool valid_;
    uint64_t leaf_;
    int64_t idx_;
    char key_[MaxKeyLen];
    const void* val_;
  };

  Cursor SeekRange(const void* lo, bool lo_incl, const void* hi,
                   bool hi_incl) const noexcept {
    Cursor cur(this, hi, hi_incl);
    if (slot_->root_off == 0) return cur;
    const int lc = KeyCmp(lo, hi, MaxKeyLen);
    if (lc > 0 || (lc == 0 && !(lo_incl && hi_incl))) return cur;
    uint64_t off = slot_->root_off;
    while (true) {
      const NodeHeader* n = Node(off);
      const int pos =
          n->leaf != 0 ? LowerBound(off, lo, n->count)
                       : StrictUpper(off, lo, n->count);
      if (n->leaf != 0) {
        cur.leaf_ = off;
        cur.idx_ = lo_incl ? pos : StrictUpper(off, lo, n->count);
        LoadAndCheckHi(&cur);
        return cur;
      }
      off = ChildAt(off, pos);
    }
  }

 private:
  friend class Cursor;

  // ------------------------ 布局与底层访问 ------------------------
  struct NodeHeader {
    uint16_t count;      ///< 实体数（内部=分隔键数；叶=条目数）
    uint8_t leaf;
    uint8_t level;
    uint16_t rsv;
    uint64_t next_leaf;
  };
  static constexpr size_t kHdr = 16;
  static constexpr size_t kLeafStride =
      detail::AlignUp8(MaxKeyLen + sizeof(Payload));
  static constexpr size_t kIntStride = detail::AlignUp8(MaxKeyLen + 8);
  static constexpr int kCapLeaf =
      static_cast<int>((kNodeBytes - kHdr) / kLeafStride);
  static constexpr int kCapInt =
      static_cast<int>((kNodeBytes - kHdr - 8) / kIntStride);
  static_assert(kCapLeaf >= 4 && kCapInt >= 4, "页面容量不足");

  template <typename T>
  T* PtrOf(uint64_t off) const noexcept {
    return reinterpret_cast<T*>(static_cast<char*>(base_) + off);
  }
  const NodeHeader* Node(uint64_t off) const noexcept {
    return PtrOf<NodeHeader>(off);
  }
  NodeHeader* MutHeader(uint64_t off) const noexcept {
    return PtrOf<NodeHeader>(off);
  }
  static int CapOf(const NodeHeader* n) noexcept {
    return n->leaf != 0 ? kCapLeaf : kCapInt;
  }

  /// 第 i 个槽的起始字节（内部=child0 之后第 i-1 槽？此处 i 从 0 计：
  /// 叶=条目区起点；内部=首个键值对，含 child0 的布局见 ChildAt）。
  char* KeyPtrAt(uint64_t off, size_t i) const noexcept {
    const NodeHeader* n = Node(off);
    return static_cast<char*>(base_) + off +
           (n->leaf != 0 ? kHdr : kHdr + 8) +
           i * (n->leaf != 0 ? kLeafStride : kIntStride);
  }
  const char* RawKeyAt(uint64_t off, int i) const noexcept {
    return KeyPtrAt(off, static_cast<size_t>(i));
  }
  void* LeafValuePtr(uint64_t off, int i) const noexcept {
    return KeyPtrAt(off, static_cast<size_t>(i)) + MaxKeyLen;
  }
  int KeyCmpAt(uint64_t off, int i, const void* key) const noexcept {
    return detail::KeyCmp(RawKeyAt(off, i), key, MaxKeyLen);
  }
  static int KeyCmp(const void* a, const void* b, size_t) noexcept {
    return detail::KeyCmp(a, b, MaxKeyLen);
  }

  uint64_t Child0(uint64_t off) const noexcept {
    uint64_t c;
    std::memcpy(&c, static_cast<const char*>(base_) + off + kHdr, 8);
    return c;
  }
  void SetChild0(uint64_t off, uint64_t v) const noexcept {
    std::memcpy(static_cast<char*>(base_) + off + kHdr, &v, 8);
  }
  /// 内部节点第 i 个孩子：0=头后专用位；i>=1 取第 i-1 槽的右孩域。
  uint64_t ChildAt(uint64_t off, int i) const noexcept {
    if (i == 0) return Child0(off);
    uint64_t c;
    std::memcpy(&c,
                KeyPtrAt(off, static_cast<size_t>(i - 1)) + MaxKeyLen, 8);
    return c;
  }
  /// 写内部节点第 i 个实体（键 + 右孩），要求 i >= 1。
  void SetChildEntry(uint64_t off, int i, const void* key,
                     uint64_t child) const noexcept {
    char* p = KeyPtrAt(off, static_cast<size_t>(i - 1));
    std::memcpy(p, key, MaxKeyLen);
    std::memcpy(p + MaxKeyLen, &child, 8);
  }

  int LowerBound(uint64_t off, const void* target, int count) const noexcept {
    int lo = 0, hi = count;
    while (lo < hi) {
      const int mid = lo + (hi - lo) / 2;
      if (KeyCmpAt(off, mid, target) < 0)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }
  int StrictUpper(uint64_t off, const void* target, int count) const noexcept {
    int lo = 0, hi = count;
    while (lo < hi) {
      const int mid = lo + (hi - lo) / 2;
      if (KeyCmpAt(off, mid, target) <= 0)
        lo = mid + 1;
      else
        hi = mid;
    }
    return lo;
  }

  Result<uint64_t> AllocNode(uint8_t leaf, uint8_t level) const noexcept {
    Result<uint64_t> r =
        alloc_->Allocate(static_cast<uint64_t>(kNodeBytes));
    if (!r.IsOk()) return r;
    if (std::getenv("RTDB_BTREE_DEBUG") != nullptr) {
      std::printf("[allocnode] off=%llu leaf=%u lvl=%u\n",
                  static_cast<unsigned long long>(r.Value()), leaf, level);
    }
    // TEMP-DIAG：仅清头部与首段，缩小怀疑面
    std::memset(PtrOf<void>(r.Value()), 0, 64);
    MutHeader(r.Value())->leaf = leaf;
    MutHeader(r.Value())->level = level;
    return r;
  }

  void ShiftEntriesRight(uint64_t off, int pos, int n_after) const noexcept {
    if (n_after <= 0) return;
    const NodeHeader* h = Node(off);
    const size_t stride = h->leaf != 0 ? kLeafStride : kIntStride;
    char* start = static_cast<char*>(base_) + off +
                  (h->leaf != 0 ? kHdr : kHdr + 8) +
                  static_cast<size_t>(pos) * stride;
    std::memmove(start + stride, start, static_cast<size_t>(n_after) * stride);
  }
  void ShiftEntriesLeft(uint64_t off, int pos, int n_after) const noexcept {
    if (n_after <= 0) return;
    const NodeHeader* h = Node(off);
    const size_t stride = h->leaf != 0 ? kLeafStride : kIntStride;
    char* start = static_cast<char*>(base_) + off +
                  (h->leaf != 0 ? kHdr : kHdr + 8) +
                  static_cast<size_t>(pos) * stride;
    std::memmove(start - stride, start, static_cast<size_t>(n_after) * stride);
  }
  /// 叶条目 [sx,ex) 从 src 整块搬到 dst 的 dx 起（同宽 memmove）。
  void ShiftEntriesRange(uint64_t src_node, int sx, int ex,
                         uint64_t dst_node, int dx) const noexcept {
    if (ex <= sx) return;
    std::memmove(KeyPtrAt(dst_node, static_cast<size_t>(dx)),
                 KeyPtrAt(src_node, static_cast<size_t>(sx)),
                 static_cast<size_t>(ex - sx) * kLeafStride);
  }

  /// 把已满的子 child 一分为二：sep=右半最小键升入父，
  /// 新孩占用父第 idx+1 槽。主动分裂保证父必不满。
  Err SplitChild(uint64_t parent, int idx, uint64_t child) noexcept {
    NodeHeader* ph = MutHeader(parent);
    NodeHeader* ch = MutHeader(child);
    Result<uint64_t> rr = AllocNode(ch->leaf, ch->level);
    if (!rr.IsOk()) return rr.Error();
    const uint64_t right = rr.Value();
    NodeHeader* rh = MutHeader(right);

    char sep[MaxKeyLen];
    if (ch->leaf != 0) {
      const int half = ch->count / 2;  // 左留 [0,half)，右取 [half,count)
      ShiftEntriesRange(child, half, ch->count, right, 0);
      rh->count = static_cast<uint16_t>(ch->count - half);
      ch->count = static_cast<uint16_t>(half);
      rh->next_leaf = ch->next_leaf;
      ch->next_leaf = right;
      std::memcpy(sep, RawKeyAt(right, 0), MaxKeyLen);
    } else {
      const int mid = ch->count / 2;  // 第 mid 号键升为分隔键
      std::memcpy(sep, RawKeyAt(child, mid), MaxKeyLen);
      SetChild0(right, ChildAt(child, mid + 1));
      for (int j = mid + 1; j < ch->count; ++j) {
        char k[MaxKeyLen];
        uint64_t c;
        std::memcpy(k, RawKeyAt(child, j), MaxKeyLen);
        std::memcpy(&c,
                    KeyPtrAt(child, static_cast<size_t>(j)) + MaxKeyLen, 8);
        SetChildEntry(right, j - mid, k, c);  // 右孩槽从 1 起编号
      }
      rh->count = static_cast<uint16_t>(ch->count - mid - 1);
      ch->count = static_cast<uint16_t>(mid);
    }

    ShiftEntriesRight(parent, idx + 1, ph->count - idx - 1);
    SetChildEntry(parent, idx + 1, sep, right);
    ph->count++;
    if (std::getenv("RTDB_BTREE_DEBUG") != nullptr) {
      const uint64_t* w =
          static_cast<const uint64_t*>(static_cast<const void*>(
              static_cast<const char*>(base_) + parent));
      std::printf(
          "[split] parent=%llu idx=%d h0=%llx c0=%llu e0k=[%02x%02x] "
          "e0c=%llu\n",
          static_cast<unsigned long long>(parent), idx,
          static_cast<unsigned long long>(w[0]),
          static_cast<unsigned long long>(w[1]), w[2] & 0xFF,
          (w[2] >> 8) & 0xFF, static_cast<unsigned long long>(w[3]));
    }
    return Err::kOk;
  }

  /// 游标定位：吸收空叶跨链推进；越上界即失效。
  void LoadAndCheckHi(Cursor* c) const noexcept {
    while (true) {
      const NodeHeader* n = Node(c->leaf_);
      if (static_cast<int64_t>(n->count) <= c->idx_) {
        const uint64_t next = n->next_leaf;
        if (next == 0) {
          c->valid_ = false;
          return;
        }
        c->leaf_ = next;
        c->idx_ = 0;
        continue;
      }
      std::memcpy(c->key_, RawKeyAt(c->leaf_, static_cast<int>(c->idx_)),
                  MaxKeyLen);
      const int cmp = detail::KeyCmp(c->key_, c->hi_, MaxKeyLen);
      if (cmp > 0 || (cmp == 0 && !c->hi_incl_)) {
        c->valid_ = false;
        return;
      }
      c->val_ = LeafValuePtr(c->leaf_, static_cast<int>(c->idx_));
      c->valid_ = true;
      return;
    }
  }

  void* base_;
  SlabShmAllocator* alloc_;
  Slot* slot_;
};

// ------------------------ 结构校验（测试/诊断用）------------------------
// 深度受限显式栈 DFS：检查层级单调、节点内严格递增、容量上界、
// 叶链完整性以及跨叶有序性。发现任一违例即 kIncompatibleLayout。
template <size_t K, typename P>
Err SharedBTree<K, P>::Validate(uint64_t* visited_nodes) const noexcept {
  if (visited_nodes != nullptr) *visited_nodes = 0;
  if (slot_->root_off == 0) return Err::kOk;
  struct Frame {
    uint64_t off;
    int level;
  };
  constexpr int kMaxDepth = 32;
  Frame stack[kMaxDepth];
  int sp = 0;
  stack[sp++] = {slot_->root_off, static_cast<int>(Node(slot_->root_off)->level)};

  char prev_last[K];
  uint64_t expect_next_leaf = 0;
  bool have_prev_leaf_key = false;
  bool expecting_chain = false;

  while (sp > 0) {
    const Frame f = stack[--sp];
    const NodeHeader* n = Node(f.off);
    if (visited_nodes != nullptr) (*visited_nodes)++;
    if (static_cast<int>(n->level) != f.level ||
        f.level < 0 || f.level >= kMaxDepth)
      return Err::kIncompatibleLayout;
    const int cap = n->leaf != 0 ? kCapLeaf : kCapInt;
    if (n->count > cap) return Err::kIncompatibleLayout;

    for (int i = 0; i + 1 < static_cast<int>(n->count); ++i) {
      if (detail::KeyCmp(RawKeyAt(f.off, i), RawKeyAt(f.off, i + 1), K) >= 0)
        return Err::kIncompatibleLayout;
    }
    if (expecting_chain && n->leaf != 0 && f.off != expect_next_leaf)
      return Err::kIncompatibleLayout;  // 叶链断裂
    if (have_prev_leaf_key && n->leaf != 0 && n->count > 0 &&
        detail::KeyCmp(prev_last, RawKeyAt(f.off, 0), K) >= 0)
      return Err::kIncompatibleLayout;  // 跨叶有序性

    if (n->leaf != 0) {
      if (n->count > 0) {
        std::memcpy(prev_last, RawKeyAt(f.off, n->count - 1), K);
        have_prev_leaf_key = true;
      }
      expect_next_leaf = n->next_leaf;
      expecting_chain = n->next_leaf != 0;
      continue;
    }
    if (f.level == 0) return Err::kIncompatibleLayout;
    // 进入内部子树：链期望挂起，待该子树首叶恢复校验。
    expecting_chain = false;
    if (sp + static_cast<int>(n->count) + 1 > kMaxDepth)
      return Err::kIncompatibleLayout;
    for (int i = static_cast<int>(n->count); i >= 0; --i)
      stack[sp++] = {ChildAt(f.off, i), f.level - 1};
  }
  if (expecting_chain) return Err::kIncompatibleLayout;  // 链必须以 0 收尾
  return Err::kOk;
}

}  // namespace infra
}  // namespace rtdb

#endif  // RTDB_SRC_INFRA_SHARED_BTREE_HPP_
