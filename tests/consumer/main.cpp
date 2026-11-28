#include <clab/categorical_code.hpp>
#include <clab/categorical_heldout.hpp>
#include <clab/categorical_motif.hpp>
#include <clab/categorical_temporal.hpp>
#include <clab/evaluator.hpp>
#include <clab/histogram_cache.hpp>
#include <clab/lattice_view.hpp>
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
    const clab::RelationalSnapshot lattice(
        std::array<clab::EntityRow, 2>{{{20, 19}, {10, 11}}},
        std::array<clab::RelationRow, 1>{{{1, 0, 0, 2, false}}},
        std::array<clab::PortRow, 2>{{{20, 0}, {10, 0}}});
    const auto lattice_view = clab::categorical_lattice_view(
        lattice, 1, 2, std::array<clab::EntityId, 2>{10, 20});
    if (!std::ranges::equal(lattice_view.symbols(), std::array<clab::TypeId, 2>{11, 19})) {
        return 1;
    }
    const auto encoded = clab::encode_categorical(witness, clab::CategoricalCode::periodic);
    if (clab::decode_categorical(encoded.bytes, encoded.length.total_bits()) !=
        std::vector<clab::TypeId>(witness.begin(), witness.end())) {
        return 1;
    }
    const auto iid = clab::encode_categorical_iid(witness);
    if (!std::ranges::equal(clab::decode_categorical_iid(iid.bytes, iid.length.total_bits()), witness)) {
        return 1;
    }
    const auto comparison = clab::compare_categorical(witness);
    if (!comparison.winner ||
        !std::ranges::equal(clab::decode_selected_categorical(
            comparison.winner->encoded.bytes, comparison.winner->encoded.length.total_bits()), witness)) {
        return 1;
    }
    const std::array<clab::TypeId, 4> motif_witness{11, 19, 11, 19};
    const auto motif = clab::compare_categorical_motif(motif_witness);
    if (!motif.motif || !std::ranges::equal(clab::decode_categorical_motif(
            motif.motif->encoded.bytes, motif.motif->encoded.length.total_bits()), motif_witness)) {
        return 1;
    }
    const std::array<clab::TypeId, 12> dictionary_witness{1, 2, 9, 3, 4, 8, 1, 2, 7, 3, 4, 6};
    const auto dictionary = clab::compare_categorical_motif_dictionary(dictionary_witness);
    if (!dictionary.dictionary ||
        !std::ranges::equal(clab::decode_categorical_motif_dictionary(
            dictionary.dictionary->encoded.bytes, dictionary.dictionary->encoded.length.total_bits()),
            dictionary_witness)) {
        return 1;
    }
    const auto frozen_dictionary = clab::FrozenCategoricalMotifDictionary::fit(dictionary_witness);
    if (!frozen_dictionary) {
        return 1;
    }
    const auto& dictionary_model = frozen_dictionary->model_code();
    const auto restored_dictionary = clab::FrozenCategoricalMotifDictionary::from_bytes(
        dictionary_model.bytes, dictionary_model.length.total_bits());
    const auto dictionary_test = frozen_dictionary->encode(dictionary_witness);
    if (!std::ranges::equal(restored_dictionary.decode(dictionary_test.encoded.bytes,
                                                       dictionary_test.encoded.length.total_bits()),
                           dictionary_witness)) {
        return 1;
    }
    const auto frozen_motif = clab::FrozenCategoricalMotif::from_bytes(
        std::array<std::uint8_t, 9>{0x40, 0, 0, 0, 0x20, 0, 0, 0, 0x40}, 67);
    const std::array<clab::TypeId, 4> frozen_witness{1, 2, 1, 2};
    const auto motif_test = frozen_motif.encode(frozen_witness);
    if (!std::ranges::equal(frozen_motif.decode(motif_test.code.bytes, motif_test.code.length.total_bits()),
                           frozen_witness)) {
        return 1;
    }
    const auto fitted = clab::FrozenCategoricalModel::fit(witness);
    const auto& model = fitted.model_code();
    const auto restored = clab::FrozenCategoricalModel::from_bytes(model.bytes, model.length.total_bits());
    const auto heldout = fitted.encode(witness);
    if (!std::ranges::equal(restored.decode(heldout.code.bytes, heldout.code.length.total_bits()), witness)) {
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
    clab::TraceReader temporal_trace(path);
    const auto temporal = clab::score_categorical_temporal(temporal_trace, {1, 1, 1, 2});
    if (temporal.status != clab::MetricStatus::insufficient_data || temporal.train_seen != 1) {
        return 1;
    }
    clab::TraceReader motif_temporal_trace(path);
    const auto motif_temporal = clab::score_categorical_motif_temporal(motif_temporal_trace, {1, 1, 1, 2});
    if (motif_temporal.status != clab::MetricStatus::insufficient_data || motif_temporal.train_seen != 1) {
        return 1;
    }
    clab::TraceReader dictionary_temporal_trace(path);
    const auto dictionary_temporal = clab::score_categorical_motif_dictionary_temporal(
        dictionary_temporal_trace, {1, 1, 1, 2});
    if (dictionary_temporal.status != clab::MetricStatus::insufficient_data || dictionary_temporal.train_seen != 1) {
        return 1;
    }
    for (auto score : {clab::score_categorical_motif_permutations, clab::score_categorical_motif_site_permutations}) {
        clab::TraceReader permutation_trace(path);
        const auto permutation = score(permutation_trace, {1, 1, 1, 2}, 1, 0, {});
        if (permutation.observed.status != clab::MetricStatus::insufficient_data ||
            !permutation.draws.empty() || permutation.at_least_observed) {
            return 1;
        }
    }
    clab::TraceReader dictionary_permutation_trace(path);
    const auto dictionary_permutation = clab::score_categorical_motif_dictionary_site_permutations(
        dictionary_permutation_trace, {1, 1, 1, 2}, 1, 0);
    if (dictionary_permutation.observed.status != clab::MetricStatus::insufficient_data ||
        !dictionary_permutation.draws.empty() || dictionary_permutation.at_least_observed) {
        return 1;
    }
    const auto stats = cache.stats();
    return stats.stores + stats.hits == 1 && found_entropy && found_entity && trace.status() == clab::TraceStatus::complete ? 0 : 1;
}
