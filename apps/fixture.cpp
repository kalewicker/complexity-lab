#include <clab/substrate.hpp>
#include <clab/trace.hpp>

#include <fstream>
#include <iostream>

namespace {
void write_text(const std::filesystem::path& path, const char* content) {
    std::ofstream out(path);
    out << content;
    out.close();
    if (!out) {
        throw clab::Error(clab::ErrorCode::io, "fixture manifest write failed");
    }
}
}

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: clab-fixture NEW_OUTPUT_DIRECTORY\n";
        return 2;
    }
    try {
        const std::filesystem::path directory(argv[1]);
        if (!std::filesystem::create_directory(directory)) {
            throw clab::Error(clab::ErrorCode::io, "fixture output directory already exists");
        }
        constexpr clab::WorkCount target = 8, cap = 5;
        constexpr std::uint64_t seed = 0, memory_cap = 128;
        constexpr std::size_t sites = 9;
        const char* run_id = "mock-work-cap";
        std::ofstream planned(directory / "planned.csv");
        planned << "run_id,target_work,hard_work_cap,sites,seed,cell_buffer_cap\n"
                << run_id << ',' << target << ',' << cap << ',' << sites << ',' << seed
                << ',' << memory_cap << '\n';
        planned.close();
        if (!planned) {
            throw clab::Error(clab::ErrorCode::io, "planned manifest write failed");
        }
        clab::WorkCount work = 0;
        try {
            clab::BinaryCa world(sites, seed, memory_cap);
            clab::TraceWriter trace(directory / "trace.cltr",
                                    {run_id, "rule90-ring-u32-v1", "sweep", true, true});
            std::uint64_t sequence = 0;
            do {
                trace.append({{sequence++, static_cast<double>(work), work}, world.observe()});
                trace.append({{sequence++, static_cast<double>(work), work}, world.ledger()});
                if (work == target || !world.advance(cap)) {
                    break;
                }
                ++work;
            } while (true);
            trace.finish();
            std::ofstream status(directory / "status.csv");
            status << "run_id,status,reason,completed_work,trace\n" << run_id << ','
                   << (work == target ? "completed,," : "capped,work,")
                   << work << ",trace.cltr\n";
            status.close();
            if (!status) {
                throw clab::Error(clab::ErrorCode::io, "status write failed");
            }
        } catch (...) {
            write_text(directory / "status.csv",
                       "run_id,status,reason,completed_work,trace\nmock-work-cap,failed,exception,,\n");
            throw;
        }
    } catch (const std::exception& error) {
        std::cerr << "clab-fixture: " << error.what() << '\n';
        return 1;
    }
}
