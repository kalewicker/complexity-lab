#include <clab/categorical_code.hpp>
#include <clab/evaluator.hpp>
#include <clab/histogram_cache.hpp>
#include <clab/trace.hpp>

#include <iomanip>
#include <iostream>
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
              << "       clab-eval --categorical-baselines TRACE.cltr\n";
    return 2;
}

void checked_flush(std::ostream& out) {
    out.flush();
    if (!out) {
        throw clab::Error(clab::ErrorCode::io, "CSV output failed");
    }
}

void report_categorical_baselines(clab::TraceReader& trace, std::ostream& out) {
    out.imbue(std::locale::classic());
    out << "run_id,sequence,work,encoding,exact,symbol_count,code_id,"
           "header_bits,model_bits,data_bits,total_bits,status\n";
    const auto write_row = [&](clab::CategoricalCode code, const clab::ObservationRecord* record,
                               std::size_t count, std::optional<clab::CodeLength> length,
                               clab::MetricStatus status) {
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
        out << ',' << clab::categorical_code_id(code) << ',';
        if (length) {
            out << length->header_bits << ',' << length->model_bits << ',' << length->data_bits
                << ',' << length->total_bits();
        } else {
            out << ",,,";
        }
        out << ',' << status_name(status) << '\n';
    };

    bool saw_categorical = false;
    while (const auto record = trace.next()) {
        const auto* snapshot = std::get_if<clab::Snapshot>(&record->payload);
        if (!snapshot) {
            continue;
        }
        saw_categorical = true;
        const auto symbols = snapshot->symbols();
        for (const auto code : clab::categorical_codes) {
            try {
                const auto encoded = clab::encode_categorical(symbols, code);
                write_row(code, &*record, symbols.size(), encoded.length, clab::MetricStatus::valid);
            } catch (const clab::Error& error) {
                if (error.code() != clab::ErrorCode::limit) {
                    throw;
                }
                write_row(code, &*record, symbols.size(), std::nullopt, clab::MetricStatus::budget_exceeded);
            }
        }
    }
    if (!saw_categorical) {
        for (const auto code : clab::categorical_codes) {
            write_row(code, nullptr, 0, std::nullopt, clab::MetricStatus::unsupported);
        }
    }
    checked_flush(out);
}
}

int main(int argc, char** argv) {
    try {
        auto& out = std::cout;
        if (argc > 1 && std::string_view(argv[1]) == "--categorical-baselines") {
            if (argc != 3) {
                return usage();
            }
            clab::TraceReader trace(argv[2]);
            report_categorical_baselines(trace, out);
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
