#include <clab/trace.hpp>

#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
    const bool recover = argc == 3 && std::string_view(argv[1]) == "--recover-prefix";
    if (argc != 2 && !recover) {
        std::cerr << "usage: clab-inspect [--recover-prefix] TRACE.cltr\n";
        return 2;
    }
    try {
        clab::TraceReader trace(argv[argc - 1], recover ? clab::TraceMode::recover_prefix :
                                                        clab::TraceMode::strict);
        std::uint64_t records = 0;
        while (trace.next()) {
            ++records;
        }
        const bool complete = trace.status() == clab::TraceStatus::complete;
        std::cout << "status=" << (complete ? "complete" : "incomplete")
                  << "\nrecords=" << records << "\nchunks=" << trace.chunks_read() << '\n';
        return complete ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "clab-inspect: " << error.what() << '\n';
        return 1;
    }
}
