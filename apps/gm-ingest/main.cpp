// gm-ingest: Price ingestion, caching, and validation (ADR-002, ADR-015)
//
// M0 STUB. This stage does not yet do real work - it exists so the full
// chain (gm-run) wires end-to-end and produces a trivial artifact on
// both build platforms, per the M0 exit criterion in ADR.md §13. The
// real implementation lands in the milestone named in the description
// above (see ADR.md for which one).

#include <gm-core/stage_main.hpp>

#include <fstream>

namespace {

gm::VoidResult run_gm_ingest(const gm::Config& /*config*/, const std::filesystem::path& output_dir,
                           gm::Manifest& manifest) {
    std::error_code ec;
    std::filesystem::create_directories(output_dir, ec);
    if (ec) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kIoFailure,
                                               "failed to create output directory",
                                               output_dir.string()));
    }

    std::filesystem::path stub_path = output_dir / "gm-ingest.stub.json";
    std::ofstream out(stub_path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return tl::unexpected(gm::Error::make(gm::ErrorCode::kIoFailure,
                                               "failed to write stub artifact", stub_path.string()));
    }
    out << "{\n  \"stage\": \"gm-ingest\",\n  \"status\": \"stub\",\n"
        << "  \"note\": \"M0 skeleton - see ADR.md milestones for real implementation\"\n}\n";

    manifest.set_string("stub_artifact", stub_path.string());
    manifest.set_int("rows_written", 0);
    return {};
}

} // namespace

int main(int argc, char** argv) {
    return gm::run_stage_main(argc, argv, "gm-ingest", run_gm_ingest);
}
