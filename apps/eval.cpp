#include <clab/evaluator.hpp>
#include <clab/trace.hpp>

#include <iomanip>
#include <iostream>
#include <locale>
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
    std::cerr << "usage: clab-eval [--metric ID]... TRACE.cltr\n"
              << "       clab-eval --list-metrics\n"
              << "       clab-eval --plan-metrics [--metric ID]...\n";
    return 2;
}

void checked_flush(std::ostream& out) {
    out.flush();
    if (!out) {
        throw clab::Error(clab::ErrorCode::io, "CSV output failed");
    }
}
}

int main(int argc, char** argv) {
    try {
        auto& out = std::cout;
        if (argc == 2 && std::string_view(argv[1]) == "--list-metrics") {
            out << "metric_id,channel,unit\n";
            for (const auto& metric : clab::metric_catalog()) {
                out << metric.id << ',' << metric.channel << ',' << metric.unit << '\n';
            }
            checked_flush(out);
            return 0;
        }
        std::vector<std::string_view> selected;
        const bool print_plan = argc > 1 && std::string_view(argv[1]) == "--plan-metrics";
        int arg = print_plan ? 2 : 1;
        while (arg < argc && std::string_view(argv[arg]) == "--metric") {
            if (arg + (print_plan ? 1 : 2) >= argc) {
                return usage();
            }
            const std::string_view id = argv[arg + 1];
            if (!clab::find_metric(id)) {
                std::cerr << "clab-eval: unknown metric ID: " << id << '\n';
                return 2;
            }
            selected.push_back(id);
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
        clab::evaluate(trace, [&](const clab::MetricRow& row) {
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
        }, {}, selected);
        checked_flush(out);
    } catch (const std::exception& error) {
        std::cerr << "clab-eval: " << error.what() << '\n';
        return 1;
    }
}
