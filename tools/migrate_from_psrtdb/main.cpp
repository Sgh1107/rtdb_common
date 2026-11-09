/// \file
/// \brief M4-W3 `migrate_from_psrtdb` CLI（docs/16 D8）。
///
/// 用法：
///   migrate_from_psrtdb --source <fixture> --mapping <json> --target <image>
///                       [--dry-run] [--limit N] [--batch N] [--report <json>]
///   migrate_from_psrtdb --make-fixture <path>
///
/// 退出码：0 = 成功（报告 passed=true）；1 = 迁移/校验/IO 失败；2 = 用法错误。
/// 核心逻辑在 migrate_core.hpp（单测直接包含，不依赖本文件的 main）。

#include "migrate_core.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "platform/console.hpp"

namespace {

namespace mg = rtdb::migrate;

void Usage() {
    std::fprintf(stderr,
                 "用法：migrate_from_psrtdb --source <fixture> --mapping <json> "
                 "--target <image>\n"
                 "       [--dry-run] [--limit N] [--batch N] [--report <json>]\n"
                 "       migrate_from_psrtdb --make-fixture <path>\n"
                 "  --dry-run 只解析 mapping/fixture 并预估行数，不创建目标映像\n"
                 "  --limit N 每表最多迁移 N 行；--batch N 原子批行数（默认 512）\n"
                 "  --size-mb N 新建目标映像大小（默认 256，容量规划见 docs/16 D8）\n"
                 "  --report  报告写入文件（缺省打印到 stdout）\n"
                 "  源映像格式为自描述 fixture；真实 PSRtdb 适配器实现 "
                 "IPsrImageAdapter 即可替换。\n");
}

}  // namespace

int main(int argc, char** argv) {
    rtdb::platform::UseUtf8Console();
    mg::MigrateParams p;
    std::string make_fixture;
    bool help = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string* out) -> bool {
            if (i + 1 >= argc) return false;
            *out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--source" && next(&v)) p.source = std::move(v);
        else if (a == "--mapping" && next(&v)) p.mapping = std::move(v);
        else if (a == "--target" && next(&v)) p.target = std::move(v);
        else if (a == "--report" && next(&v)) p.report = std::move(v);
        else if (a == "--limit" && next(&v)) p.limit = std::strtoull(v.c_str(), nullptr, 10);
        else if (a == "--batch" && next(&v)) p.batch_rows = std::strtoull(v.c_str(), nullptr, 10);
        else if (a == "--size-mb" && next(&v)) p.target_size_mb = std::strtoull(v.c_str(), nullptr, 10);
        else if (a == "--dry-run") p.dry_run = true;
        else if (a == "--make-fixture" && next(&v)) make_fixture = std::move(v);
        else if (a == "--help" || a == "-h") help = true;
        else { std::fprintf(stderr, "未知参数：%s\n", a.c_str()); Usage(); return 2; }
    }
    if (help) { Usage(); return 0; }

    if (!make_fixture.empty()) {
        std::string err;
        if (!mg::MakeFixture(make_fixture, &err)) {
            std::fprintf(stderr, "生成 fixture 失败：%s\n", err.c_str());
            return 1;
        }
        std::printf("已生成 fixture：%s\n", make_fixture.c_str());
        return 0;
    }
    if (p.source.empty() || p.mapping.empty() || (!p.dry_run && p.target.empty())) {
        Usage();
        return 2;
    }

    std::string err;
    mg::MigrationReport rep;
    const rtdb::Err e = mg::Run(p, &rep, &err);
    if (e != rtdb::Err::kOk) {
        std::fprintf(stderr, "迁移失败（%s）：%s\n", rtdb::ErrorMessage(e), err.c_str());
        return 1;
    }
    // 报告写文件已由 mg::Run 完成（CLI 与单测共用同一路径）。
    if (p.report.empty()) {
        std::printf("%s\n", mg::ReportJson(rep).c_str());
    } else {
        std::printf("报告已写入：%s\n", p.report.c_str());
    }
    std::printf("%s：%llu 张表，耗时 %llu ms\n", p.dry_run ? "DRY-RUN OK" : "MIGRATION PASS",
                static_cast<unsigned long long>(rep.tables.size()),
                static_cast<unsigned long long>(rep.elapsed_ms));
    return rep.passed ? 0 : 1;
}
