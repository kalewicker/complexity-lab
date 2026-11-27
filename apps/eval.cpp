#include <clab/categorical_code.hpp>
#include <clab/categorical_heldout.hpp>
#include <clab/evaluator.hpp>
#include <clab/histogram_cache.hpp>
#include <clab/trace.hpp>

#include <charconv>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
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
              << "       clab-eval --categorical-heldout TRAIN GUARD TEST TRACE.cltr\n";
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

std::optional<std::size_t> parse_size(std::string_view input) {
    std::size_t size{};
    const auto end = input.data() + input.size();
    const auto parsed = std::from_chars(input.data(), end, size);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return std::nullopt;
    }
    return size;
}

struct HeldOutSplit {
    std::size_t training, guard, test;
};

void report_heldout(clab::TraceReader& trace, std::ostream& out, HeldOutSplit split) {
    out.imbue(std::locale::classic());
    out << "run_id,sequence,work,encoding,exact,symbol_count,train_symbols,guard_symbols,test_symbols,"
           "code_id,model_bits,test_header_bits,test_data_bits,test_bits,raw_bits,unseen_symbols,"
           "conditional_gain_bits,charged_gain_bits,status\n";
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
            << clab::categorical_frozen_code_id << ',';
    };
    const auto write_missing = [&](clab::MetricStatus status) {
        out << ",,,,,,,," << status_name(status) << '\n';
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
            write_missing(clab::MetricStatus::insufficient_data);
            continue;
        }
        try {
            const auto fitted = clab::FrozenCategoricalModel::fit(symbols.first(split.training));
            const auto test = symbols.subspan(split.training + split.guard, split.test);
            const auto encoded = fitted.encode(test);
            const auto model_bits = fitted.model_code().length.total_bits();
            const auto test_bits = encoded.code.length.total_bits();
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
            write_missing(clab::MetricStatus::budget_exceeded);
        }
    }
    if (!saw_categorical) {
        write_prefix(nullptr, 0);
        write_missing(clab::MetricStatus::unsupported);
    }
    checked_flush(out);
}

}

int main(int argc, char** argv) {
    try {
        auto& out = std::cout;
        const auto mode = argc > 1 ? std::string_view(argv[1]) : std::string_view{};
        if (mode == "--categorical-heldout") {
            if (argc != 6) {
                return usage();
            }
            const auto train = parse_size(argv[2]), guard = parse_size(argv[3]), test = parse_size(argv[4]);
            if (!train || !guard || !test || *train == 0 || *test == 0 ||
                *guard > std::numeric_limits<std::size_t>::max() - *train ||
                *test > std::numeric_limits<std::size_t>::max() - (*train + *guard)) {
                std::cerr << "clab-eval: invalid categorical region sizes\n";
                return 2;
            }
            clab::TraceReader trace(argv[5]);
            report_heldout(trace, out, {*train, *guard, *test});
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
