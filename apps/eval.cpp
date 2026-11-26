#include <clab/evaluator.hpp>
#include <clab/trace.hpp>

#include <iomanip>
#include <iostream>
#include <locale>
#include <string_view>

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
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: clab-eval TRACE.cltr\n";
        return 2;
    }
    try {
        clab::TraceReader trace(argv[1]);
        auto& out = std::cout;
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
        });
        out.flush();
        if (!out) {
            throw clab::Error(clab::ErrorCode::io, "CSV output failed");
        }
    } catch (const std::exception& error) {
        std::cerr << "clab-eval: " << error.what() << '\n';
        return 1;
    }
}
