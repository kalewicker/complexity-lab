#include "checkpoint.hpp"

#include <clab/trace.hpp>

#include <charconv>

#include <iomanip>
#include <iostream>
#include <locale>
#include <string_view>
#include <utility>

namespace {
enum class Output { summary, index, checkpoints, metadata };

int verify_checkpoint(std::string_view sequence_arg, const std::filesystem::path& target,
                      const std::filesystem::path& trace_path) {
    std::uint64_t sequence{};
    const auto end = sequence_arg.data() + sequence_arg.size();
    const auto parsed = std::from_chars(sequence_arg.data(), end, sequence);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        std::cerr << "clab-inspect: invalid checkpoint sequence\n";
        return 2;
    }

    clab::TraceReader trace(trace_path);
    std::optional<clab::CheckpointRef> reference;
    while (const auto record = trace.next()) {
        if (record->stamp.sequence == sequence) {
            if (const auto* ref = std::get_if<clab::CheckpointRef>(&record->payload)) {
                reference = *ref;
            }
        }
    }
    if (!reference) {
        throw clab::Error(clab::ErrorCode::invalid, "no checkpoint reference at requested sequence");
    }
    clab::verify_checkpoint_target(*reference, target);
    std::cout << "status=verified\nsequence=" << sequence << '\n' << std::flush;
    if (!std::cout) {
        throw clab::Error(clab::ErrorCode::io, "inspection output failed");
    }
    return 0;
}

void usage() {
    std::cerr << "usage: clab-inspect [--recover-prefix] [--index|--checkpoints|--metadata] TRACE.cltr\n"
              << "       clab-inspect --verify-checkpoint SEQUENCE CHECKPOINT_FILE TRACE.cltr\n";
}
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--verify-checkpoint") {
        if (argc != 5) {
            usage();
            return 2;
        }
        try {
            return verify_checkpoint(argv[2], argv[3], argv[4]);
        } catch (const std::exception& error) {
            std::cerr << "clab-inspect: " << error.what() << '\n';
            return 1;
        }
    }
    bool recover = false;
    Output output = Output::summary;
    int arg = 1;
    for (; arg < argc - 1; ++arg) {
        const std::string_view flag = argv[arg];
        if (flag == "--recover-prefix" && !recover) {
            recover = true;
        } else if (flag == "--index" && output == Output::summary) {
            output = Output::index;
        } else if (flag == "--checkpoints" && output == Output::summary) {
            output = Output::checkpoints;
        } else if (flag == "--metadata" && output == Output::summary) {
            output = Output::metadata;
        } else {
            break;
        }
    }
    if (argc < 2 || arg != argc - 1) {
        usage();
        return 2;
    }
    try {
        clab::ChunkIndexSink sink;
        if (output == Output::index) {
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
        if (output == Output::checkpoints) {
            std::cout << "sequence,work,format,size_bytes,sha256\n";
        }
        std::uint64_t records = 0;
        while (auto record = trace.next()) {
            ++records;
            if (output == Output::checkpoints) {
                if (const auto* ref = std::get_if<clab::CheckpointRef>(&record->payload)) {
                    constexpr std::string_view hex = "0123456789abcdef";
                    std::cout << record->stamp.sequence << ',' << record->stamp.work << ','
                              << ref->format << ',' << ref->size_bytes << ',';
                    for (const auto byte : ref->sha256) {
                        std::cout << hex[byte >> 4] << hex[byte & 0xf];
                    }
                    std::cout << '\n';
                }
            }
        }
        const bool complete = trace.status() == clab::TraceStatus::complete;
        if (output == Output::summary) {
            std::cout << "status=" << (complete ? "complete" : "incomplete")
                      << "\nrecords=" << records << "\nchunks=" << trace.chunks_read() << '\n';
        } else if (output == Output::metadata) {
            std::cout << clab::encode_metadata_json(trace.metadata()) << '\n';
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
