#include <clab/categorical_code.hpp>
#include <clab/evaluator.hpp>
#include <clab/histogram_cache.hpp>
#include <clab/trace.hpp>

#include <algorithm>
#include <array>
#include <filesystem>

int main(int argc, char** argv) {
    if (argc != 2) {
        return 1;
    }
    const auto* entropy_spec = clab::find_metric("state.shannon_bits_per_symbol.v1");
    if (!entropy_spec || entropy_spec->unit != "bits_per_symbol" || clab::metric_catalog().size() != 27) {
        return 1;
    }
    const std::array<std::string_view, 1> selected{entropy_spec->id};
    if (clab::plan_metrics(selected) !=
        std::vector<std::string>{"artifact.symbol_histogram.v1", std::string(entropy_spec->id)}) {
        return 1;
    }
    const std::array<clab::TypeId, 3> witness{11, 19, 11};
    const auto encoded = clab::encode_categorical(witness, clab::CategoricalCode::periodic);
    if (clab::decode_categorical(encoded.bytes, encoded.length.total_bits()) !=
        std::vector<clab::TypeId>(witness.begin(), witness.end())) {
        return 1;
    }
    const auto iid = clab::encode_categorical_iid(witness);
    if (!std::ranges::equal(clab::decode_categorical_iid(iid.bytes, iid.length.total_bits()), witness)) {
        return 1;
    }
    const std::filesystem::path path(argv[1]);
    const clab::TraceProvenance provenance{
        std::string(64, 'a'), std::nullopt, "external-consumer.v1", "cpu", "u32",
        {true, true, false, true, false, false, clab::TimeDomain::discrete},
    };
    const std::array schema{clab::AttributeDescriptor{
        clab::AttributeOwner::entity, "temperature", true,
        clab::ContinuousAttribute{"K", 250, 400, 0.25},
    }};
    const clab::Metadata meta{"consumer", "binary-u32", "step", true, true, provenance,
                              {schema.begin(), schema.end()}};
    const std::array values{clab::AttributeColumn{clab::AttributeValue(300.25)}};
    clab::TraceWriter writer(path, meta);
    writer.append({{}, clab::Snapshot(std::array<clab::TypeId, 2>{0, 1})});
    writer.append({{1, 0, 0}, clab::RelationalSnapshot(
        std::array<clab::EntityRow, 1>{{{7, 0}}}, {}, {}, values, schema)});
    writer.finish();
    clab::TraceReader trace(path);
    clab::HistogramCache cache(path.parent_path() / (path.filename().string() + ".cache"));
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
    }, {}, {}, cache);
    const auto stats = cache.stats();
    return stats.stores + stats.hits == 1 && found_entropy && found_entity && trace.status() == clab::TraceStatus::complete ? 0 : 1;
}
