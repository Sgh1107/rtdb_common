/// M0 诊断：最小复现 SharedBTree 二次插入时节点页被清零的问题。
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <random>
#include <vector>

#include "infra/shared_btree.hpp"

namespace {

namespace infra = rtdb::infra;

struct Region {
    static constexpr uint64_t kBytes = 4ull << 20;
    Region() : mem(static_cast<char*>(::operator new(kBytes, std::align_val_t(4096)))) {
        std::memset(mem, 0, kBytes);
    }
    ~Region() { ::operator delete(mem, std::align_val_t(4096)); }
    char* mem;
};

using Tree = infra::SharedBTree<8, uint64_t>;

void DumpHeader(const char* tag, const Region& r, uint64_t off) {
    // 仅打印头部字节（count/leaf/level/next），不触碰私有类型。
    const unsigned char* b = reinterpret_cast<const unsigned char*>(r.mem + off);
    const unsigned cnt = b[0] | (b[1] << 8);
    std::printf("%s off=%llu count=%u leaf=%u level=%u next=%llu\n", tag,
                static_cast<unsigned long long>(off), cnt, b[2], b[3],
                static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(b + 8)));
}

}  // namespace

int main() {
    Region region;
    auto alloc_res = infra::SlabShmAllocator::Attach(region.mem, Region::kBytes, true);
    if (!alloc_res.IsOk()) {
        std::printf("attach fail\n");
        return 1;
    }
    auto alloc = alloc_res.TakeValue();

    Tree::Slot slot{0, 0};
    Tree tree(region.mem, alloc.get(), &slot);

    char k1[8], k2[8];
    infra::EncodeOrderedInt64(500, k1);
    infra::EncodeOrderedInt64(424, k2);

    const rtdb::Err e1 = tree.Insert(k1, 111);
    std::printf("insert1 err=%d\n", static_cast<int>(e1));
    DumpHeader("after-ins1", region, slot.root_off);

    const rtdb::Err e2 = tree.Insert(k2, 222);
    std::printf("insert2 err=%d\n", static_cast<int>(e2));
    DumpHeader("after-ins2", region, slot.root_off);

    uint64_t out = 0;
    std::printf("find500=%d find424=%d size=%llu\n", static_cast<int>(tree.Find(k1, &out)),
                static_cast<int>(tree.Find(k2, &out)),
                static_cast<unsigned long long>(tree.Size()));

    // ---- 阶段2：600 个乱序插入（复现 gtest 失败序列），出错即转储 ----
    std::vector<int> idx(7500);
    for (int i = 0; i < 7500; ++i) idx[i] = i + 1000;  // 与阶段1键空间不相交
    std::mt19937_64 rng(42);
    std::shuffle(idx.begin(), idx.end(), rng);

    auto dump_internal = [&](const char* tag, uint64_t off) {
        const unsigned char* b = reinterpret_cast<const unsigned char*>(region.mem + off);
        const unsigned cnt = b[0] | (b[1] << 8);
        std::printf("%s INT off=%llu cnt=%u lvl=%u child0=%llu\n", tag,
                    static_cast<unsigned long long>(off), cnt, b[3],
                    static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(b + 16)));
        for (unsigned s = 0; s < cnt && s < 4; ++s) {
            const unsigned char* p = b + 24 + s * 16;
            std::printf("   pair[%u] key=%02x%02x child=%llu\n", s, p[0], p[1],
                        static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(p + 8)));
        }
    };

    for (int step = 0; step < 7500; ++step) {
        const int k = idx[step];
        char kk[8];
        infra::EncodeOrderedInt64(k, kk);
        const rtdb::Err e = tree.Insert(kk, static_cast<uint64_t>(k));
        if (e != rtdb::Err::kOk) {
            std::printf("FAIL at step=%d key=%d err=%d root=%llu\n", step, k, static_cast<int>(e),
                        static_cast<unsigned long long>(slot.root_off));
            return 2;
        }
        if ((step % 500) == 499) {
            uint64_t vis = 0;
            const rtdb::Err vr = tree.Validate(&vis);
            std::printf("validate@%d => %d visited=%llu\n", step + 1, static_cast<int>(vr),
                        static_cast<unsigned long long>(vis));
            if (vr != rtdb::Err::kOk) {
                // 细化：叶链有序性检查
                uint64_t leaf = slot.root_off;
                int lvl = 8;
                while (lvl-- > 0) {
                    const unsigned char* b =
                        reinterpret_cast<const unsigned char*>(region.mem + leaf);
                    if ((b[2] & 0xFF) != 0) break;
                    leaf = *reinterpret_cast<const uint64_t*>(b + 16);
                }
                // leaf 现为最左叶
                unsigned char prev_last[8] = {0};
                bool have = false;
                uint64_t expect = 0;
                bool chain = false;
                int leafno = 0;
                while (true) {
                    const unsigned char* b =
                        reinterpret_cast<const unsigned char*>(region.mem + leaf);
                    const unsigned cnt = b[0] | (b[1] << 8);
                    if (cnt > 0) {
                        const unsigned char* first = b + 16;
                        const unsigned char* last = b + 16 + (cnt - 1) * 16;
                        if (have && std::memcmp(prev_last, first, 8) >= 0) {
                            std::printf("CROSS-LEAF ORDER BREAK at leaf=%d\n", leafno);
                            std::printf("  prev_last=%02x%02x first=%02x%02x\n", prev_last[0],
                                        prev_last[7], first[0], first[7]);
                            break;
                        }
                        std::memcpy(prev_last, last, 8);
                        have = true;
                    }
                    const uint64_t nx = *reinterpret_cast<const uint64_t*>(b + 8);
                    ++leafno;
                    if (nx == 0) break;
                    expect = nx;
                    leaf = nx;
                    (void)chain;
                }
                // 根 pairs 升序检查（假设根为 lvl>=1 内部）
                const unsigned char* rb =
                    reinterpret_cast<const unsigned char*>(region.mem + slot.root_off);
                const unsigned rc = rb[0] | (rb[1] << 8);
                int bad = -1;
                for (unsigned s = 1; s < rc; ++s) {
                    if (std::memcmp(rb + 24 + (s - 1) * 16, rb + 24 + s * 16, 8) >= 0) {
                        bad = static_cast<int>(s);
                        break;
                    }
                }
                std::printf("root pairs: cnt=%u asc-violating-slot=%d\n", rc, bad);
                for (unsigned s = 0; s < rc && s < 10; ++s) {
                    const unsigned char* p = rb + 24 + s * 16;
                    std::printf(
                        "  pair[%u] key=%02x %02x %02x %02x %02x %02x %02x %02x child=%llu\n", s,
                        p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                        static_cast<unsigned long long>(*reinterpret_cast<const uint64_t*>(p + 8)));
                }
                return 6;
            }
        }
        // 零孩哨兵：全树 DFS，任何内部节点的孩子指针不得为 0。
        {
            std::function<void(uint64_t, int)> walk = [&](uint64_t off, int depth) {
                if (off == 0 || depth > 8) {
                    std::printf("BAD-NODE off=%llu depth=%d step=%d\n",
                                static_cast<unsigned long long>(off), depth, step);
                    std::exit(5);
                }
                const unsigned char* b = reinterpret_cast<const unsigned char*>(region.mem + off);
                const unsigned cnt = b[0] | (b[1] << 8);
                if ((b[2] & 0xFF) != 0) return;  // leaf
                const uint64_t c0 = *reinterpret_cast<const uint64_t*>(b + 16);
                if (c0 == 0) {
                    std::printf("ZERO-child0 node=%llu step=%d key=%d cnt=%u\n",
                                static_cast<unsigned long long>(off), step, k, cnt);
                    for (unsigned s = 0; s < cnt; ++s) {
                        const unsigned char* p = b + 24 + s * 16;
                        std::printf("  pair[%u] key=%02x%02x child=%llu\n", s, p[0], p[1],
                                    static_cast<unsigned long long>(
                                        *reinterpret_cast<const uint64_t*>(p + 8)));
                    }
                    std::exit(5);
                }
                walk(c0, depth + 1);
                for (unsigned s = 0; s < cnt; ++s) {
                    const uint64_t ch = *reinterpret_cast<const uint64_t*>(b + 24 + s * 16 + 8);
                    if (ch == 0) {
                        std::printf("ZERO-pair[%u] node=%llu step=%d key=%d cnt=%u\n", s,
                                    static_cast<unsigned long long>(off), step, k, cnt);
                        std::exit(5);
                    }
                    walk(ch, depth + 1);
                }
            };
            walk(slot.root_off, 0);
        }
    }
    std::printf("ALL 600 INSERTS OK size=%llu\n", static_cast<unsigned long long>(tree.Size()));
    return 0;
}
