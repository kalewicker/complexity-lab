#include <clab/evaluator.hpp>
#include <clab/trace.hpp>

#include <array>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }
    const std::filesystem::path path(argv[1]);
    const clab::TraceProvenance provenance{
        std::string(64, 'a'), std::nullopt, "external-consumer.v1", "cpu", "u32",
        {true, true, false, true, false, false, clab::TimeDomain::discrete},
    };
    const clab::Metadata meta{"consumer", "binary-u32", "step", true, true, provenance};
    clab::TraceWriter writer(path, meta);
    writer.append({{}, clab::Snapshot(std::array<clab::TypeId, 2>{0, 1})});
    writer.append({{1, 0, 0}, clab::RelationalSnapshot(
        std::array<clab::EntityRow, 1>{{{7, 0}}}, {}, {})});
    writer.finish();
    clab::TraceReader trace(path);
    if (trace.metadata() != meta) {
        return 1;
    }
    bool found_entropy = false, found_entity = false;
    clab::evaluate(trace, [&](const clab::MetricRow& row) {
        if (row.id == "state.shannon_bits_per_symbol.v1") {
            found_entropy = row.value && std::get<double>(*row.value) == 1.0;
        }
        if (row.id == "relational.entity_count.v1") {
            found_entity = row.value && std::get<std::uint64_t>(*row.value) == 1;
        }
    });
    return found_entropy && found_entity && trace.status() == clab::TraceStatus::complete ? 0 : 1;
}
