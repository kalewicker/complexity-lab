#include <clab/evaluator.hpp>
#include <clab/trace.hpp>

#include <array>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }
    const std::filesystem::path path(argv[1]);
    clab::TraceWriter writer(path, {"consumer", "binary-u32", "step", true, true});
    writer.append({{}, clab::Snapshot(std::array<clab::TypeId, 2>{0, 1})});
    writer.finish();
    clab::TraceReader trace(path);
    bool found = false;
    clab::evaluate(trace, [&](const clab::MetricRow& row) {
        if (row.id == "state.shannon_bits_per_symbol.v1") {
            found = row.value && std::get<double>(*row.value) == 1.0;
        }
    });
    return found && trace.status() == clab::TraceStatus::complete ? 0 : 1;
}
