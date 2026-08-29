/// W1 存储基座单测：DataType 定长/对齐表、PlanRowLayout 纯函数
/// （列偏移自然对齐排布、变长槽、长度覆盖校验、版本数组/墓碑位图尺寸）。

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "storage/schema.hpp"

namespace {

using rtdb::storage::AlignOf;
using rtdb::storage::ColDefHost;
using rtdb::storage::DataType;
using rtdb::storage::FixedSizeOf;
using rtdb::storage::PlanRowLayout;
using rtdb::storage::RowLayoutPlan;

// ---------------------------------------------------------------------------
// 类型表：定长宽度与自然对齐
// ---------------------------------------------------------------------------
TEST(StorageSchema, FixedSizesMatchTypeTable) {
    EXPECT_EQ(FixedSizeOf(DataType::kInt8), 1u);
    EXPECT_EQ(FixedSizeOf(DataType::kInt16), 2u);
    EXPECT_EQ(FixedSizeOf(DataType::kInt32), 4u);
    EXPECT_EQ(FixedSizeOf(DataType::kInt64), 8u);
    EXPECT_EQ(FixedSizeOf(DataType::kFloat32), 4u);
    EXPECT_EQ(FixedSizeOf(DataType::kFloat64), 8u);
    EXPECT_EQ(FixedSizeOf(DataType::kBool), 1u);
    EXPECT_EQ(FixedSizeOf(DataType::kUtf8Var), 0u);
    EXPECT_EQ(FixedSizeOf(DataType::kBytesVar), 0u);
    EXPECT_EQ(FixedSizeOf(DataType::kUnixTsNs), 8u);
    // 未知类型值必须安全返回 0（防御持久化脏数据）。
    EXPECT_EQ(FixedSizeOf(static_cast<DataType>(99)), 0u);
}

TEST(StorageSchema, AlignOfNaturalAlignment) {
    EXPECT_EQ(AlignOf(DataType::kInt8), 1u);
    EXPECT_EQ(AlignOf(DataType::kBool), 1u);
    EXPECT_EQ(AlignOf(DataType::kInt16), 2u);
    EXPECT_EQ(AlignOf(DataType::kInt32), 4u);
    EXPECT_EQ(AlignOf(DataType::kFloat32), 4u);
    EXPECT_EQ(AlignOf(DataType::kInt64), 8u);
    EXPECT_EQ(AlignOf(DataType::kFloat64), 8u);
    EXPECT_EQ(AlignOf(DataType::kUnixTsNs), 8u);
    // 变长列记录内为 (u32 off, u32 len) 槽，按 8 对齐。
    EXPECT_EQ(AlignOf(DataType::kUtf8Var), 8u);
    EXPECT_EQ(AlignOf(DataType::kBytesVar), 8u);
    EXPECT_EQ(AlignOf(static_cast<DataType>(99)), 1u);
}

// ---------------------------------------------------------------------------
// PlanRowLayout：混合列的自然对齐排布
// ---------------------------------------------------------------------------
TEST(StorageSchema, PlanRowLayoutMixedColumnOffsets) {
    // i64, f64, utf8var, i32, bool —— 依次自然对齐：
    //   i64@0(8)  f64@8(8)  var@16(8 槽)  i32@24(4)  bool@28(1) → 尾部对齐 32
    const std::vector<ColDefHost> cols = {
        {"a_int64", DataType::kInt64}, {"b_f64", DataType::kFloat64},
        {"c_str", DataType::kUtf8Var}, {"d_i32", DataType::kInt32},
        {"e_flag", DataType::kBool},
    };
    std::vector<uint32_t> off(cols.size());
    const RowLayoutPlan plan = PlanRowLayout(cols.data(), cols.size(), 100, off.data());
    ASSERT_EQ(plan.err, rtdb::Err::kOk);
    EXPECT_EQ(off[0], 0u);
    EXPECT_EQ(off[1], 8u);
    EXPECT_EQ(off[2], 16u);
    EXPECT_EQ(off[3], 24u);
    EXPECT_EQ(off[4], 28u);
    EXPECT_EQ(plan.rec_fixed_len, 32u);  // AlignUp8(29)
    EXPECT_EQ(plan.var_col_count, 1u);
    EXPECT_EQ(plan.ver_array_bytes, 800u);  // 100 × 8
    EXPECT_EQ(plan.tombmap_bytes, 13u);     // ceil(100/8)
}

TEST(StorageSchema, PlanRowLayoutAllVariableColumns) {
    const std::vector<ColDefHost> cols = {
        {"s1", DataType::kUtf8Var},
        {"b1", DataType::kBytesVar},
    };
    const RowLayoutPlan plan = PlanRowLayout(cols.data(), cols.size(), 7);
    ASSERT_EQ(plan.err, rtdb::Err::kOk);
    EXPECT_EQ(plan.rec_fixed_len, 16u);  // 两个 8 字节 (off,len) 槽
    EXPECT_EQ(plan.var_col_count, 2u);
    EXPECT_EQ(plan.ver_array_bytes, 56u);
    EXPECT_EQ(plan.tombmap_bytes, 1u);  // ceil(7/8)
}

TEST(StorageSchema, PlanRowLayoutLenOverrideWidensColumn) {
    // 定长覆盖：kInt8 + len=16 视作 16 字节二进制定长字段。
    const std::vector<ColDefHost> cols = {
        {"blob16", DataType::kInt8, /*len=*/16},
        {"tail", DataType::kInt32},
    };
    std::vector<uint32_t> off(2);
    const RowLayoutPlan plan = PlanRowLayout(cols.data(), cols.size(), 4, off.data());
    ASSERT_EQ(plan.err, rtdb::Err::kOk);
    EXPECT_EQ(off[0], 0u);
    EXPECT_EQ(off[1], 16u);
    EXPECT_EQ(plan.rec_fixed_len, 24u);  // 20 → AlignUp8
}

// ---------------------------------------------------------------------------
// PlanRowLayout：非法输入拒绝
// ---------------------------------------------------------------------------
TEST(StorageSchema, PlanRowLayoutRejectsInvalidInput) {
    // 空列序列。
    EXPECT_EQ(PlanRowLayout(nullptr, 3, 10).err, rtdb::Err::kInvalidArgument);
    // len 覆盖小于类型固有宽度。
    const std::vector<ColDefHost> bad = {{"x", DataType::kInt64, /*len=*/4}};
    EXPECT_EQ(PlanRowLayout(bad.data(), bad.size(), 10).err, rtdb::Err::kInvalidArgument);
}

TEST(StorageSchema, PlanRowLayoutRowCapBoundaries) {
    const std::vector<ColDefHost> cols = {{"v", DataType::kInt64}};
    struct Cap {
        uint32_t cap;
        uint64_t ver;
        uint64_t tomb;
    };
    // 墓碑位图按位计：cap 1/7/8/9 → 1/1/1/2 字节。
    const Cap cases[] = {{1, 8, 1}, {7, 56, 1}, {8, 64, 1}, {9, 72, 2}};
    for (const Cap& c : cases) {
        const RowLayoutPlan plan = PlanRowLayout(cols.data(), cols.size(), c.cap);
        ASSERT_EQ(plan.err, rtdb::Err::kOk);
        EXPECT_EQ(plan.ver_array_bytes, c.ver) << "row_cap=" << c.cap;
        EXPECT_EQ(plan.tombmap_bytes, c.tomb) << "row_cap=" << c.cap;
    }
}

// ---------------------------------------------------------------------------
// shm 结构体布局锚点（跨编译器防漂移；单测与 CI 双平台共同约束）
// ---------------------------------------------------------------------------
TEST(StorageSchema, ShmStructSizesPinned) {
    EXPECT_EQ(sizeof(rtdb::storage::shm_ColDef), 48u);
    EXPECT_EQ(sizeof(rtdb::storage::shm_TableDescriptor), 1280u);
}

}  // namespace
