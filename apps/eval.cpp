#include <clab/categorical_code.hpp>
#include <clab/categorical_heldout.hpp>
#include <clab/categorical_motif.hpp>
#include <clab/categorical_temporal.hpp>
#include <clab/evaluator.hpp>
#include <clab/histogram_cache.hpp>
#include <clab/trace.hpp>

#include <algorithm>
#include <charconv>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
void csv_text(std::ostream& out, std::string_view value) {
    out << '"';
    for (auto ch : value) {
        if (ch == '"') {
            out << '"';
        }
        out << ch;
    }
    out << '"';
}

const char* status_name(clab::MetricStatus status) {
    switch (status) {
    case clab::MetricStatus::valid: return "valid";
    case clab::MetricStatus::unsupported: return "unsupported";
    case clab::MetricStatus::insufficient_data: return "insufficient_data";
    case clab::MetricStatus::budget_exceeded: return "budget_exceeded";
    }
    throw std::logic_error("unknown metric status");
}

int usage() {
    std::cerr << "usage: clab-eval [--metric ID]... [--cache-dir DIRECTORY] TRACE.cltr\n"
              << "       clab-eval --list-metrics\n"
              << "       clab-eval --plan-metrics [--metric ID]...\n"
              << "       clab-eval --categorical-baselines TRACE.cltr\n"
              << "       clab-eval --categorical-iid TRACE.cltr\n"
              << "       clab-eval --categorical-compare TRACE.cltr\n"
              << "       clab-eval --categorical-motif TRACE.cltr\n"
              << "       clab-eval --categorical-motif-heldout TRAIN GUARD TEST TRACE.cltr\n"
              << "       clab-eval --categorical-heldout TRAIN GUARD TEST TRACE.cltr\n"
              << "       clab-eval --categorical-motif-temporal TRAIN GUARD TEST SITES TRACE.cltr\n"
              << "       clab-eval --categorical-motif-permutation TRAIN GUARD TEST SITES DRAWS SEED TRACE.cltr\n"
              << "       clab-eval --categorical-temporal TRAIN GUARD TEST SITES TRACE.cltr\n";
    return 2;
}

void checked_flush(std::ostream& out) {
    out.flush();
    if (!out) {
        throw clab::Error(clab::ErrorCode::io, "CSV output failed");
    }
}

enum class CategoricalReport { baselines, iid, comparison };

void report_categorical_baselines(clab::TraceReader& trace, std::ostream& out, CategoricalReport mode) {
    const bool compare = mode == CategoricalReport::comparison;
    out.imbue(std::locale::classic());
    out << "run_id,sequence,work,encoding,exact,symbol_count,code_id,"
           "header_bits,model_bits,data_bits,total_bits,status";
    if (compare) {
        out << ",family_bits,comparison_status,selected";
    }
    out << '\n';
    const auto write_row = [&](std::string_view code_id, const clab::ObservationRecord* record,
                               std::size_t count, std::optional<clab::CodeLength> length,
                               clab::MetricStatus status,
                               clab::MetricStatus comparison_status = clab::MetricStatus::unsupported,
                               bool selected = false) {
        const auto& meta = trace.metadata();
        csv_text(out, meta.run_id);
        out << ',';
        if (record) {
            out << record->stamp.sequence << ',' << record->stamp.work;
        } else {
            out << ',';
        }
        out << ',';
        csv_text(out, meta.encoding);
        out << ',' << (meta.exact ? "true" : "false") << ',';
        if (record) {
            out << count;
        }
        out << ',' << code_id << ',';
        if (length) {
            out << length->header_bits << ',' << length->model_bits << ',' << length->data_bits
                << ',' << length->total_bits();
        } else {
            out << ",,,";
        }
        out << ',' << status_name(status);
        if (compare) {
            out << ',' << clab::categorical_family_bits << ',' << status_name(comparison_status)
                << ',' << (selected ? "true" : "false");
        }
        out << '\n';
    };

    bool saw_categorical = false;
    while (const auto record = trace.next()) {
        const auto* snapshot = std::get_if<clab::Snapshot>(&record->payload);
        if (!snapshot) {
            continue;
        }
        saw_categorical = true;
        const auto symbols = snapshot->symbols();
        if (compare) {
            const auto result = clab::compare_categorical(symbols);
            const auto comparison_status = result.winner ? clab::MetricStatus::valid
                                                         : clab::MetricStatus::budget_exceeded;
            for (const auto& candidate : result.candidates) {
                write_row(clab::categorical_family_id(candidate.family), &*record, symbols.size(),
                          candidate.length,
                          candidate.length ? clab::MetricStatus::valid : clab::MetricStatus::budget_exceeded,
                          comparison_status, result.winner && result.winner->family == candidate.family);
            }
            continue;
        }
        const auto report = [&](std::string_view id, const auto& encode) {
            try {
                const auto encoded = encode(symbols);
                write_row(id, &*record, symbols.size(), encoded.length, clab::MetricStatus::valid);
            } catch (const clab::Error& error) {
                if (error.code() != clab::ErrorCode::limit) {
                    throw;
                }
                write_row(id, &*record, symbols.size(), std::nullopt, clab::MetricStatus::budget_exceeded);
            }
        };
        if (mode == CategoricalReport::iid) {
            report(clab::categorical_iid_code_id,
                   [](auto input) { return clab::encode_categorical_iid(input); });
        } else {
            for (const auto code : clab::categorical_codes) {
                report(clab::categorical_code_id(code),
                       [code](auto input) { return clab::encode_categorical(input, code); });
            }
        }
    }
    if (!saw_categorical) {
        if (mode != CategoricalReport::iid) {
            for (const auto code : clab::categorical_codes) {
                write_row(clab::categorical_code_id(code), nullptr, 0, std::nullopt,
                          clab::MetricStatus::unsupported);
            }
        }
        if (mode != CategoricalReport::baselines) {
            write_row(clab::categorical_iid_code_id, nullptr, 0, std::nullopt, clab::MetricStatus::unsupported);
        }
    }
    checked_flush(out);
}

void report_categorical_motifs(clab::TraceReader& trace, std::ostream& out) {
    out.imbue(std::locale::classic());
    out << "run_id,sequence,work,encoding,exact,symbol_count,baseline_id,baseline_bits,motif_id,"
           "motif_width,occurrences,header_bits,model_bits,placement_bits,residual_bits,total_bits,"
           "gain_bits,selected,status\n";
    const auto write_row = [&](const clab::ObservationRecord* record, std::size_t count,
                               const clab::CategoricalMotifComparison* result, std::string_view status) {
        const auto& meta = trace.metadata();
        csv_text(out, meta.run_id);
        out << ',';
        if (record) {
            out << record->stamp.sequence << ',' << record->stamp.work;
        } else {
            out << ',';
        }
        out << ',';
        csv_text(out, meta.encoding);
        out << ',' << (meta.exact ? "true" : "false") << ',';
        if (record) {
            out << count;
        }
        out << ',';
        if (result) {
            out << clab::categorical_family_id(result->baseline.family) << ','
                << result->baseline.encoded.length.total_bits();
        } else {
            out << ',';
        }
        out << ',' << clab::categorical_motif_code_id << ',';
        if (result && result->motif) {
            const auto& motif = *result->motif;
            const auto& length = motif.encoded.length;
            const auto gain = static_cast<std::int64_t>(result->baseline.encoded.length.total_bits())
                              - static_cast<std::int64_t>(length.total_bits());
            out << motif.pattern.size() << ',' << motif.placements.size() << ',' << length.header_bits
                << ',' << length.model_bits << ',' << motif.placement_bits << ','
                << length.data_bits - motif.placement_bits << ',' << length.total_bits() << ',' << gain;
        } else {
            out << ",,,,,,,";
        }
        out << ',' << (result && result->improves_baseline() ? "true" : "false") << ',' << status << '\n';
    };

    bool saw_categorical = false;
    while (const auto record = trace.next()) {
        const auto* snapshot = std::get_if<clab::Snapshot>(&record->payload);
        if (!snapshot) {
            continue;
        }
        saw_categorical = true;
        const auto symbols = snapshot->symbols();
        try {
            const auto result = clab::compare_categorical_motif(symbols);
            write_row(&*record, symbols.size(), &result, result.motif ? "valid" : "no_motif");
        } catch (const clab::Error& error) {
            if (error.code() != clab::ErrorCode::limit) {
                throw;
            }
            write_row(&*record, symbols.size(), nullptr, "budget_exceeded");
        }
    }
    if (!saw_categorical) {
        write_row(nullptr, 0, nullptr, "unsupported");
    }
    checked_flush(out);
}

template <typename UInt = std::size_t>
std::optional<UInt> parse_unsigned(std::string_view input) {
    UInt value{};
    const auto end = input.data() + input.size();
    const auto parsed = std::from_chars(input.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return value;
}

struct HeldOutSplit {
    std::size_t training, guard, test;
};

void report_heldout(clab::TraceReader& trace, std::ostream& out, HeldOutSplit split, bool motif) {
    out.imbue(std::locale::classic());
    out << "run_id,sequence,work,encoding,exact,symbol_count,train_symbols,guard_symbols,test_symbols,code_id,";
    if (motif) {
        out << "baseline_id,model_bits,test_header_bits,placement_bits,residual_bits,test_bits,occurrences,"
               "baseline_model_bits,baseline_test_bits,conditional_gain_bits,charged_gain_bits,status\n";
    } else {
        out << "model_bits,test_header_bits,test_data_bits,test_bits,raw_bits,unseen_symbols,"
               "conditional_gain_bits,charged_gain_bits,status\n";
    }
    const auto minimum = clab::checked_add(clab::checked_add(split.training, split.guard), split.test);
    bool saw_categorical = false;
    const auto write_prefix = [&](const clab::ObservationRecord* record, std::size_t count) {
        const auto& meta = trace.metadata();
        csv_text(out, meta.run_id);
        out << ',';
        if (record) {
            out << record->stamp.sequence << ',' << record->stamp.work;
        } else {
            out << ',';
        }
        out << ',';
        csv_text(out, meta.encoding);
        out << ',' << (meta.exact ? "true" : "false") << ',';
        if (record) {
            out << count;
        }
        out << ',' << split.training << ',' << split.guard << ',' << split.test << ','
            << (motif ? clab::categorical_frozen_motif_code_id : clab::categorical_frozen_code_id) << ',';
        if (motif) {
            out << clab::categorical_frozen_code_id << ',';
        }
    };
    const auto write_missing = [&](std::string_view status) {
        out << (motif ? ",,,,,,,,,," : ",,,,,,,,") << status << '\n';
    };
    while (const auto record = trace.next()) {
        const auto* snapshot = std::get_if<clab::Snapshot>(&record->payload);
        if (!snapshot) {
            continue;
        }
        saw_categorical = true;
        const auto symbols = snapshot->symbols();
        write_prefix(&*record, symbols.size());
        if (symbols.size() < minimum) {
            write_missing("insufficient_data");
            continue;
        }
        try {
            const auto training = symbols.first(split.training);
            const auto test = symbols.subspan(split.training + split.guard, split.test);
            if (motif && test.size() > clab::categorical_iid_symbol_limit) {
                write_missing("budget_exceeded");
                continue;
            }
            const auto fitted = clab::FrozenCategoricalModel::fit(training);
            const auto encoded = fitted.encode(test);
            const auto model_bits = fitted.model_code().length.total_bits();
            const auto test_bits = encoded.code.length.total_bits();
            if (motif) {
                const auto dictionary = clab::FrozenCategoricalMotif::fit(training);
                if (!dictionary) {
                    out << ",,,,,," << model_bits << ',' << test_bits << ",,,no_training_motif\n";
                    continue;
                }
                const auto motif_test = dictionary->encode(test);
                const auto motif_model_bits = dictionary->model_code().length.total_bits();
                const auto& test_length = motif_test.code.length;
                const auto conditional = static_cast<std::int64_t>(test_bits)
                                       - static_cast<std::int64_t>(test_length.total_bits());
                const auto charged = conditional + static_cast<std::int64_t>(model_bits)
                                   - static_cast<std::int64_t>(motif_model_bits);
                out << motif_model_bits << ',' << test_length.header_bits << ',' << motif_test.placement_bits
                    << ',' << test_length.data_bits - motif_test.placement_bits << ',' << test_length.total_bits()
                    << ',' << motif_test.placements.size() << ',' << model_bits << ',' << test_bits
                    << ',' << conditional << ',' << charged << ",valid\n";
                continue;
            }
            // The raw comparator uses the same length frame and external family choice.
            const auto raw_bits = clab::checked_add(encoded.code.length.header_bits,
                                                    clab::checked_multiply(test.size(), 32));
            const auto conditional_gain = static_cast<std::int64_t>(raw_bits) - static_cast<std::int64_t>(test_bits);
            const auto charged_gain = conditional_gain - static_cast<std::int64_t>(model_bits);
            out << model_bits << ',' << encoded.code.length.header_bits << ','
                << encoded.code.length.data_bits << ',' << test_bits << ',' << raw_bits << ','
                << encoded.unseen_symbols << ',' << conditional_gain << ',' << charged_gain << ",valid\n";
        } catch (const clab::Error& error) {
            if (error.code() != clab::ErrorCode::limit) {
                throw;
            }
            write_missing("budget_exceeded");
        }
    }
    if (!saw_categorical) {
        write_prefix(nullptr, 0);
        write_missing("unsupported");
    }
    checked_flush(out);
}


template <typename Result>
void write_temporal_prefix(std::ostream& out, const clab::Metadata& meta, clab::TemporalCategoricalSplit split,
                           const Result& result, std::string_view code_id, std::string_view cost_columns) {
    out.imbue(std::locale::classic());
    out << std::setprecision(std::numeric_limits<double>::max_digits10)
        << "run_id,encoding,time_unit,exact,code_id,train_snapshots,guard_snapshots,test_snapshots,sites,"
           "train_seen,guard_seen,test_seen,train_end_sequence,train_end_work,train_end_time,"
           "test_start_sequence,test_start_work,test_start_time,test_end_sequence,test_end_work,"
           "test_end_time," << cost_columns << ",status,reason\n";
    csv_text(out, meta.run_id);
    out << ',';
    csv_text(out, meta.encoding);
    out << ',';
    csv_text(out, meta.time_unit);
    out << ',' << (meta.exact ? "true" : "false") << ',' << code_id
        << ',' << split.train_snapshots << ',' << split.guard_snapshots << ',' << split.test_snapshots
        << ',' << split.sites << ',' << result.train_seen << ',' << result.guard_seen << ',' << result.test_seen;
    for (const auto& stamp : {result.train_end, result.test_start, result.test_end}) {
        out << ',';
        if (stamp) {
            out << stamp->sequence << ',' << stamp->work << ',' << stamp->simulated_time;
        } else {
            out << ",,";
        }
    }
}

void report_temporal(clab::TraceReader& trace, std::ostream& out, clab::TemporalCategoricalSplit split) {
    const auto result = clab::score_categorical_temporal(trace, split);
    write_temporal_prefix(out, trace.metadata(), split, result, clab::categorical_frozen_code_id,
                          "model_bits,test_header_bits,test_data_bits,test_bits,raw_bits,"
                          "unseen_symbols,conditional_gain_bits,charged_gain_bits");
    if (const auto& costs = result.costs) {
        const auto model = costs->model.length.total_bits();
        const auto test = clab::checked_add(costs->test_header_bits, costs->test_data_bits);
        const auto gain = static_cast<std::int64_t>(costs->raw_bits) - static_cast<std::int64_t>(test);
        out << ',' << model << ',' << costs->test_header_bits << ',' << costs->test_data_bits
            << ',' << test << ',' << costs->raw_bits << ',' << costs->unseen_symbols
            << ',' << gain << ',' << gain - static_cast<std::int64_t>(model);
    } else {
        out << ",,,,,,,,";
    }
    out << ',' << status_name(result.status) << ',' << result.reason << '\n';
    checked_flush(out);
}

struct PermutationOptions {
    std::size_t count;
    std::uint64_t seed;
};

void write_permutation_costs(std::ostream& out, const clab::TemporalMotifPermutationResult& result,
                             PermutationOptions options) {
    std::size_t complete = 0, absent = 0;
    for (const auto& draw : result.draws) {
        if (draw.status == clab::MetricStatus::valid) {
            ++complete;
            absent += !draw.charged_gain_bits;
        }
    }
    out << ',' << clab::temporal_motif_permutation_id << ',' << options.seed << ',' << options.count
        << ',' << complete << ',' << absent << ',' << result.draws.size() - complete << ',';
    if (result.observed.costs) {
        out << std::max<std::int64_t>(0, result.observed.costs->charged_gain_bits().value_or(0));
    }
    if (result.at_least_observed) {
        out << ',' << *result.at_least_observed << ',' << *result.at_least_observed + 1 << ',' << options.count + 1;
    } else {
        out << ",,,";
    }
    out << ',';
    for (std::size_t i = 0; i < result.draws.size(); ++i) {
        if (i) {
            out << ' ';
        }
        const auto& draw = result.draws[i];
        if (draw.charged_gain_bits) {
            out << *draw.charged_gain_bits;
        } else {
            out << (draw.status == clab::MetricStatus::valid ? "absent" : status_name(draw.status));
        }
    }
}

void report_motif_temporal(clab::TraceReader& trace, std::ostream& out, clab::TemporalCategoricalSplit split,
                           std::optional<PermutationOptions> options = std::nullopt) {
    const auto evaluation = options
        ? clab::score_categorical_motif_permutations(trace, split, options->count, options->seed)
        : clab::TemporalMotifPermutationResult{clab::score_categorical_motif_temporal(trace, split), {}, {}};
    const auto& result = evaluation.observed;
    std::string columns = "baseline_id,model_bits,test_header_bits,placement_bits,residual_bits,test_bits,"
                          "occurrences,baseline_model_bits,baseline_test_bits,conditional_gain_bits,charged_gain_bits";
    if (options) {
        columns += ",permutation_id,seed,draws_requested,draws_complete,draws_no_training_motif,"
                   "draws_budget_exceeded,statistic_bits,extreme_draws,p_numerator,p_denominator,draw_gains_bits";
    }
    write_temporal_prefix(out, trace.metadata(), split, result, clab::categorical_frozen_motif_code_id, columns);
    out << ',' << clab::categorical_frozen_code_id;
    if (const auto& costs = result.costs) {
        const auto baseline_model = costs->baseline_model.length.total_bits();
        if (const auto& motif = costs->motif) {
            const auto model = motif->model.length.total_bits();
            const auto test = motif->test_bits();
            const auto gain = static_cast<std::int64_t>(costs->baseline_test_bits) - static_cast<std::int64_t>(test);
            out << ',' << model << ',' << motif->test_header_bits << ',' << motif->placement_bits
                << ',' << motif->residual_bits << ',' << test << ',' << motif->occurrences
                << ',' << baseline_model << ',' << costs->baseline_test_bits << ',' << gain
                << ',' << *costs->charged_gain_bits();
        } else {
            out << ",,,,,,," << baseline_model << ',' << costs->baseline_test_bits << ",,";
        }
    } else {
        out << ",,,,,,,,,,";
    }
    if (options) {
        write_permutation_costs(out, evaluation, *options);
    }
    out << ',' << status_name(result.status) << ',' << result.reason << '\n';
    checked_flush(out);
}

}

int main(int argc, char** argv) {
    try {
        auto& out = std::cout;
        const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
        if (mode == "--categorical-motif") {
            if (argc != 3) {
                return usage();
            }
            clab::TraceReader trace(argv[2]);
            report_categorical_motifs(trace, out);
            return 0;
        }
        if (mode == "--categorical-temporal" || mode == "--categorical-motif-temporal" ||
            mode == "--categorical-motif-permutation") {
            const bool permutation = mode == "--categorical-motif-permutation";
            if (argc != (permutation ? 9 : 7)) {
                return usage();
            }
            const auto train = parse_unsigned(argv[2]), guard = parse_unsigned(argv[3]);
            const auto test = parse_unsigned(argv[4]), sites = parse_unsigned(argv[5]);
            if (!train.value_or(0) || !guard.value_or(0) || !test.value_or(0) || !sites.value_or(0)) {
                std::cerr << "clab-eval: categorical temporal sizes must be positive integers\n";
                return 2;
            }
            std::optional<PermutationOptions> options;
            if (permutation) {
                const auto draws = parse_unsigned(argv[6]);
                const auto seed = parse_unsigned<std::uint64_t>(argv[7]);
                if (!draws.value_or(0) || *draws > clab::temporal_motif_permutation_limit || !seed) {
                    std::cerr << "clab-eval: invalid permutation draw count or seed\n";
                    return 2;
                }
                options = PermutationOptions{*draws, *seed};
            }
            clab::TraceReader trace(argv[permutation ? 8 : 6]);
            const clab::TemporalCategoricalSplit split{*train, *guard, *test, *sites};
            if (mode != "--categorical-temporal") {
                report_motif_temporal(trace, out, split, options);
            } else {
                report_temporal(trace, out, split);
            }
            return 0;
        }
        if (mode == "--categorical-heldout" || mode == "--categorical-motif-heldout") {
            if (argc != 6) {
                return usage();
            }
            const auto train = parse_unsigned(argv[2]), guard = parse_unsigned(argv[3]), test = parse_unsigned(argv[4]);
            if (!train || !guard || !test || *train == 0 || *test == 0 ||
                *guard > std::numeric_limits<std::size_t>::max() - *train ||
                *test > std::numeric_limits<std::size_t>::max() - (*train + *guard)) {
                std::cerr << "clab-eval: invalid categorical region sizes\n";
                return 2;
            }
            clab::TraceReader trace(argv[5]);
            report_heldout(trace, out, {*train, *guard, *test}, mode == "--categorical-motif-heldout");
            return 0;
        }
        if (mode == "--categorical-baselines" || mode == "--categorical-iid" ||
            mode == "--categorical-compare") {
            if (argc != 3) {
                return usage();
            }
            clab::TraceReader trace(argv[2]);
            auto report = CategoricalReport::baselines;
            if (mode == "--categorical-iid") {
                report = CategoricalReport::iid;
            } else if (mode == "--categorical-compare") {
                report = CategoricalReport::comparison;
            }
            report_categorical_baselines(trace, out, report);
            return 0;
        }
        if (argc == 2 && std::string_view(argv[1]) == "--list-metrics") {
            out << "metric_id,channel,unit\n";
            for (const auto& metric : clab::metric_catalog()) {
                out << metric.id << ',' << metric.channel << ',' << metric.unit << '\n';
            }
            checked_flush(out);
            return 0;
        }
        std::vector<std::string_view> selected;
        std::optional<clab::HistogramCache> cache;
        const bool print_plan = argc > 1 && std::string_view(argv[1]) == "--plan-metrics";
        int arg = print_plan ? 2 : 1;
        while (arg < argc) {
            const std::string_view flag = argv[arg];
            if (flag == "--metric") {
                if (arg + (print_plan ? 1 : 2) >= argc) {
                    return usage();
                }
                const std::string_view id = argv[arg + 1];
                if (!clab::find_metric(id)) {
                    std::cerr << "clab-eval: unknown metric ID: " << id << '\n';
                    return 2;
                }
                selected.push_back(id);
            } else if (flag == "--cache-dir") {
                if (print_plan || cache || arg + 2 >= argc || !argv[arg + 1][0]) {
                    return usage();
                }
                cache.emplace(argv[arg + 1]);
            } else {
                break;
            }
            arg += 2;
        }
        if (print_plan) {
            if (arg != argc) {
                return usage();
            }
            out << "kind,id\n";
            for (const auto& id : clab::plan_metrics(selected)) {
                out << (clab::find_metric(id) ? "metric" : "artifact") << ',' << id << '\n';
            }
            checked_flush(out);
            return 0;
        }
        if (arg != argc - 1) {
            return usage();
        }
        clab::TraceReader trace(argv[arg]);
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<double>::max_digits10);
        out << "run_id,sequence,work,simulated_time,time_unit,encoding,exact,metric_id,symbol,value,status\n";
        const auto write_row = [&](const clab::MetricRow& row) {
            const auto& meta = trace.metadata();
            csv_text(out, meta.run_id);
            out << ',';
            if (row.stamp) {
                out << row.stamp->sequence << ',' << row.stamp->work << ',' << row.stamp->simulated_time;
            } else {
                out << ",,";
            }
            out << ',';
            csv_text(out, meta.time_unit);
            out << ',';
            csv_text(out, meta.encoding);
            out << ',' << (meta.exact ? "true" : "false") << ',' << row.id << ',';
            if (row.symbol) {
                out << *row.symbol;
            }
            out << ',';
            if (row.value) {
                std::visit([&](auto value) { out << value; }, *row.value);
            }
            out << ',' << status_name(row.status) << '\n';
        };
        if (cache) {
            clab::evaluate(trace, write_row, {}, selected, *cache);
        } else {
            clab::evaluate(trace, write_row, {}, selected);
        }
        checked_flush(out);
        if (cache) {
            const auto stats = cache->stats();
            std::cerr << "symbol_histogram_cache hits=" << stats.hits << " misses=" << stats.misses
                      << " stores=" << stats.stores << " skipped=" << stats.skipped
                      << " write_failures=" << stats.write_failures << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << "clab-eval: " << error.what() << '\n';
        return 1;
    }
}
