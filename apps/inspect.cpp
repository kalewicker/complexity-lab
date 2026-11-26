#include <clab/trace.hpp>

#include <iomanip>
#include <iostream>
#include <locale>
#include <string_view>
#include <utility>

int main(int argc, char** argv) {
    bool recover = false;
    bool index = false;
    int arg = 1;
    for (; arg < argc - 1; ++arg) {
        const std::string_view flag = argv[arg];
        if (flag == "--recover-prefix" && !recover) {
            recover = true;
        } else if (flag == "--index" && !index) {
            index = true;
        } else {
            break;
        }
    }
    if (argc < 2 || arg != argc - 1) {
        std::cerr << "usage: clab-inspect [--recover-prefix] [--index] TRACE.cltr\n";
        return 2;
    }
    try {
        clab::ChunkIndexSink sink;
        if (index) {
            std::cout.imbue(std::locale::classic());
            std::cout << std::setprecision(std::numeric_limits<double>::max_digits10);
            std::cout << "sequence,header_offset,type,version,flags,payload_bytes,crc32c,work,simulated_time\n";
            sink = [](const clab::ChunkIndexEntry& row) {
                std::cout << row.stamp.sequence << ',' << row.header_offset << ',' << row.type << ','
                          << row.version << ',' << row.flags << ',' << row.payload_bytes << ','
                          << row.crc32c << ',' << row.stamp.work << ',' << row.stamp.simulated_time
                          << '\n';
            };
        }
        const auto mode = recover ? clab::TraceMode::recover_prefix : clab::TraceMode::strict;
        clab::TraceReader trace(argv[arg], mode, {}, std::move(sink));
        std::uint64_t records = 0;
        while (trace.next()) {
            ++records;
        }
        const bool complete = trace.status() == clab::TraceStatus::complete;
        if (!index) {
            std::cout << "status=" << (complete ? "complete" : "incomplete")
                      << "\nrecords=" << records << "\nchunks=" << trace.chunks_read() << '\n';
        }
        std::cout.flush();
        if (!std::cout) {
            throw clab::Error(clab::ErrorCode::io, "inspection output failed");
        }
        return complete ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "clab-inspect: " << error.what() << '\n';
        return 1;
    }
}
