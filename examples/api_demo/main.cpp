/// api_demo：docs/04 §3 Native API 验收程序（M2-W1，docs/10 §2-M2-D1）。
///
/// 覆盖《04》§3 示例的**非订阅段**原样跑通：伞头包含 → Options 规范字段 →
/// CreateTable（primary_key 显式声明 + secondary 建表直建）→ 单条 Upsert
/// （插入 / 合并两路径）→ Get → Delete。订阅族（Subscribe/ChangeCursor）
/// 归 M3（docs/05 §3），本程序不含；RecordBlock 列式段落在 M2-W2 追加。
///
/// 与 docs/04 §3 字面差异（决策记录见 docs/10 §2-M2-D1）：
///  - 本库标准为 C++17：docs/04 §3 的指示器初始化（designated initializers）
///    在 MSVC 需 C++20，示例按聚合赋值形态落定（GCC/Clang 的 C++17 扩展
///    不跨编译器保证，公共示例不走扩展）；
///  - `columns` 为 `std::vector<ColumnDef>`（M1 存量形态，span 化迁移成本
///    高于收益）；
///  - 变长列 `desc` 的 len 取 0：M1 要求变长列 len=0（单列长度上限约束
///    列入复评清单，docs/10 §2-M2-D5）；
///  - `max_rows_hint` 取 2048：M1 行数据区单块 ≤64KB（kMaxRowDataBytes），
///    可行行槽容量按列宽折算（本表约 32B/行 → ≤2048）；docs/04 的 1000000
///    量级提示随段目录化复评开放（docs/10 §2-M2-D5）。

#include <cstdio>
#include <vector>

#include "rtdb/rtdb.hpp"

static rtdb::Err Run(const char* image_path) {
    // 聚合赋值形态（C++17 全编译器；指示器初始化需 C++20，见文件头说明）。
    rtdb::Options opt;
    opt.instance = "api_demo";
    opt.path = image_path;
    opt.role = rtdb::Role::Standalone;
    opt.wal_fsync = rtdb::FsyncPolicy::PerSecond;
    RTDB_ASSIGN_OR_MOVE(auto eng, rtdb::Engine::Open(opt));

    // 列定义全字段显式（has_default/default_value 显式收尾，避免聚合初始化
    // 的部分字段噪音；default_value 仅在加列路径有意义）。
    // 变长列 desc 的 len=0（M1 语义，见文件头差异说明）。
    std::vector<rtdb::ColumnDef> cols = {
        {"id", rtdb::DataType::kInt64, 0, false, false, {}},
        {"val", rtdb::DataType::kFloat64, 0, true, false, {}},
        {"desc", rtdb::DataType::kUtf8Var, 0, true, false, {}},
    };
    rtdb::TableDef def;
    def.name = "measure";
    def.columns = std::move(cols);
    def.primary_key = {"id"};       // 显式声明列 0 主键（M2 规范面）
    rtdb::IndexDef by_val;
    by_val.name = "by_val";
    by_val.columns = {"val"};
    def.secondary = {by_val};       // 建表直建二级索引（M2 便利面）
    def.max_rows_hint = 2048;       // 受 64KB 行数据区约束，见文件头

    // 建表（重跑时表已存在 → 改走打开，保持示例可重复执行）。
    auto ct = eng->CreateTable(def);
    if (!ct.IsOk() && ct.Error() != rtdb::Err::kTableExists) return ct.Error();

    RTDB_ASSIGN_OR_MOVE(auto tbl, eng->OpenTable("measure"));

    // ---- 单条 upsert：插入路径（未提供列 desc 走 nullable → NULL）----
    RTDB_CHECK(tbl.Upsert(rtdb::Row{{"id", int64_t{42}}, {"val", 220.5}}));
    RTDB_ASSIGN_OR_MOVE(auto got, tbl.Get(rtdb::ColumnValue::Int(42)));
    std::printf("upsert-insert: id=%lld val=%g desc_null=%d\n",
                static_cast<long long>(got[0].as_int()), got[1].as_real(),
                got[2].is_null() ? 1 : 0);

    // ---- 单条 upsert：合并路径（所给列覆盖，未给列 val 保持 220.5）----
    RTDB_CHECK(tbl.Upsert(rtdb::Row{{"id", int64_t{42}}, {"desc", "updated"}}));
    RTDB_ASSIGN_OR_MOVE(auto got2, tbl.Get(rtdb::ColumnValue::Int(42)));
    std::printf("upsert-merge : id=%lld val=%g desc=%s\n",
                static_cast<long long>(got2[0].as_int()), got2[1].as_real(),
                got2[2].is_null() ? "(null)" : got2[2].as_bytes().c_str());

    // ---- 二级索引等值查（建表 secondary 便利面，M2-W1-a）----
    RTDB_ASSIGN_OR_MOVE(auto hits, eng->GetBySecondary(tbl, "by_val",
                                                       rtdb::ColumnValue::Real(220.5)));
    std::printf("secidx by_val=220.5 -> %zu row(s)\n", hits.size());

    // ---- 删除后读应得 kNotFound ----
    RTDB_CHECK(tbl.Delete(rtdb::ColumnValue::Int(42)));
    auto gone = tbl.Get(rtdb::ColumnValue::Int(42));
    std::printf("delete+get   : %s\n", gone.IsOk() ? "still-visible?" : "kNotFound (expected)");
    if (gone.IsOk()) return rtdb::Err::kInternal;

    std::printf("api_demo OK (instance=%s sessions=%lld committed_seq=%lld)\n", image_path,
                static_cast<long long>(eng->SessionCount()),
                static_cast<long long>(eng->CommittedSeq()));
    return rtdb::Err::kOk;
}

int main(int argc, char** argv) {
    const char* image_path = argc > 1 ? argv[1] : "api_demo.rtdb";
    const rtdb::Err err = Run(image_path);
    if (!rtdb::IsOk(err)) {
        std::fprintf(stderr, "api_demo failed: %s (%d)\n", rtdb::ErrorMessage(err),
                     static_cast<int>(err));
        return 1;
    }
    return 0;
}
