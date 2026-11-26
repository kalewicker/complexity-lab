#include <clab/substrate.hpp>
#include <clab/trace.hpp>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;
using clab::WorkCount;
using clab::Error;
using clab::ErrorCode;

constexpr WorkCount target = 8;
constexpr std::uint64_t seed = 0;
constexpr std::size_t sites = 9;

enum class Scenario { work, completed, wall_cap, crash, memory_cap };
enum class WorkerExit { ok, failed, memory_cap };

struct Config {
    Scenario scenario;
    const char* run_id;
    WorkCount work_cap;
    std::uint64_t memory_cap;
    std::chrono::milliseconds wall_cap;
};

std::optional<Config> config_for(std::string_view scenario) {
    if (scenario == "work") {
        return Config{Scenario::work, "mock-work-cap", 5, 128, 5000ms};
    }
    if (scenario == "completed") {
        return Config{Scenario::completed, "mock-completed", 8, 128, 5000ms};
    }
    if (scenario == "wall-cap") {
        return Config{Scenario::wall_cap, "mock-wall-cap", 8, 128, 1500ms};
    }
    if (scenario == "crash") {
        return Config{Scenario::crash, "mock-crash", 8, 128, 5000ms};
    }
    if (scenario == "memory-cap") {
        return Config{Scenario::memory_cap, "mock-memory-cap", 8, 71, 5000ms};
    }
    return std::nullopt;
}

void write_csv(const std::filesystem::path& path, const std::string& content) {
    const auto staging = path.string() + ".tmp";
    std::ofstream out(staging);
    out << content;
    out.close();
    if (!out) {
        throw Error(ErrorCode::io, "fixture CSV write failed");
    }
    std::filesystem::rename(staging, path);
}

WorkerExit run_worker(const Config& config, const std::filesystem::path& directory) {
    std::optional<clab::BinaryCa> world;
    try {
        world.emplace(sites, seed, config.memory_cap);
    } catch (const Error& error) {
        if (error.code() == ErrorCode::limit) {
            return WorkerExit::memory_cap;
        }
        throw;
    }

    clab::TraceWriter trace(directory / "trace.cltr",
                            {config.run_id, "rule90-ring-u32-v1", "sweep", true, true});
    std::uint64_t sequence = 0;
    WorkCount work = 0;
    for (;;) {
        trace.append({{sequence++, static_cast<double>(work), work}, world->observe()});
        trace.append({{sequence++, static_cast<double>(work), work}, world->ledger()});
        if (config.scenario == Scenario::wall_cap && work == 2) {
            for (;;) {
                ::pause();
            }
        }
        if (config.scenario == Scenario::crash && work == 3) {
            ::kill(::getpid(), SIGKILL);
            throw Error(ErrorCode::io, "fixture crash signal failed");
        }
        if (work == target || !world->advance(config.work_cap)) {
            break;
        }
        ++work;
    }
    trace.finish();
    return WorkerExit::ok;
}

pid_t wait_child(pid_t child, int& status, int options) {
    pid_t result;
    do {
        result = ::waitpid(child, &status, options);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
        throw Error(ErrorCode::io, "could not collect fixture worker status");
    }
    return result;
}

struct WorkerResult {
    int wait_status{};
    bool watchdog_sent_kill{};
    std::chrono::milliseconds elapsed{};
};

WorkerResult supervise(const Config& config, const std::filesystem::path& directory) {
    const auto start = Clock::now();
    const auto deadline = start + config.wall_cap;
    const auto child = ::fork();
    if (child < 0) {
        throw Error(ErrorCode::io, "could not fork fixture worker");
    }
    if (child == 0) {
        try {
            ::_exit(static_cast<int>(run_worker(config, directory)));
        } catch (const std::exception& error) {
            std::cerr << "clab-fixture worker: " << error.what() << '\n';
            ::_exit(static_cast<int>(WorkerExit::failed));
        }
    }

    WorkerResult result;
    for (;;) {
        if (wait_child(child, result.wait_status, WNOHANG) == child) {
            break;
        }
        const auto now = Clock::now();
        if (now >= deadline) {
            result.watchdog_sent_kill = ::kill(child, SIGKILL) == 0;
            if (!result.watchdog_sent_kill && errno != ESRCH) {
                throw Error(ErrorCode::io, "could not stop fixture worker");
            }
            wait_child(child, result.wait_status, 0);
            break;
        }
        std::this_thread::sleep_until(std::min(deadline, now + 5ms));
    }
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start);
    return result;
}

struct TraceReceipt {
    std::filesystem::path relative_path;
    std::string_view status = "missing";
    std::optional<WorkCount> work;
};

TraceReceipt recover_trace(const std::filesystem::path& directory, const Config& config) {
    TraceReceipt receipt;
    if (std::filesystem::exists(directory / "trace.cltr")) {
        receipt.relative_path = "trace.cltr";
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            const auto name = entry.path().filename();
            if (!name.string().starts_with(".cltr-") ||
                !std::filesystem::is_regular_file(entry.path() / "trace.partial")) {
                continue;
            }
            if (!receipt.relative_path.empty()) {
                throw Error(ErrorCode::invalid, "multiple staged fixture traces");
            }
            receipt.relative_path = name / "trace.partial";
        }
    }
    if (receipt.relative_path.empty()) {
        return receipt;
    }
    try {
        clab::TraceReader trace(directory / receipt.relative_path, clab::TraceMode::recover_prefix);
        if (trace.metadata().run_id != config.run_id) {
            throw Error(ErrorCode::invalid, "fixture trace run ID mismatch");
        }
        std::optional<clab::Stamp> snapshot;
        while (const auto record = trace.next()) {
            if (std::holds_alternative<clab::Snapshot>(record->payload)) {
                snapshot = record->stamp;
            } else if (const auto* ledger = std::get_if<clab::WorkLedger>(&record->payload)) {
                if (snapshot && snapshot->sequence + 1 == record->stamp.sequence &&
                    snapshot->work == record->stamp.work &&
                    ledger->logical_updates == record->stamp.work) {
                    receipt.work = record->stamp.work;
                }
                snapshot.reset();
            }
        }
        receipt.status = trace.status() == clab::TraceStatus::complete ? "complete" : "incomplete";
    } catch (const Error& error) {
        if (error.code() == ErrorCode::incomplete) {
            receipt.status = "incomplete";
        } else {
            receipt.status = "invalid";
            receipt.work.reset();
            std::cerr << "clab-fixture trace: " << error.what() << '\n';
        }
    }
    return receipt;
}

struct Outcome {
    std::string_view status, reason;
};

Outcome classify(const WorkerResult& worker, const TraceReceipt& trace, const Config& config) {
    const int result = worker.wait_status;
    if (WIFSIGNALED(result)) {
        if (worker.watchdog_sent_kill && WTERMSIG(result) == SIGKILL) {
            return {"capped", "wall"};
        }
        return {"failed", "signal"};
    }
    if (WIFEXITED(result)) {
        if (WEXITSTATUS(result) == static_cast<int>(WorkerExit::memory_cap)) {
            return {"capped", "memory"};
        }
        if (WEXITSTATUS(result) == static_cast<int>(WorkerExit::ok)) {
            if (trace.status != "complete" || trace.work != std::min(target, config.work_cap)) {
                return {"failed", "trace"};
            }
            return trace.work == target ? Outcome{"completed", ""} : Outcome{"capped", "work"};
        }
    }
    return {"failed", "worker"};
}

int run_fixture(const Config& config, const std::filesystem::path& directory) {
    if (!std::filesystem::create_directory(directory)) {
        throw Error(ErrorCode::io, "fixture output directory already exists");
    }
    std::ostringstream planned;
    planned << "run_id,target_work,hard_work_cap,sites,seed,cell_buffer_cap,wall_cap_ms\n"
            << config.run_id << ',' << target << ',' << config.work_cap << ',' << sites << ','
            << seed << ',' << config.memory_cap << ',' << config.wall_cap.count() << '\n';
    write_csv(directory / "planned.csv", planned.str());

    constexpr auto header =
        "run_id,status,reason,completed_work,trace,trace_status,worker_exit,worker_signal,elapsed_ms\n";
    try {
        const auto worker = supervise(config, directory);
        const auto trace = recover_trace(directory, config);
        const auto outcome = classify(worker, trace, config);
        std::ostringstream status;
        status << header << config.run_id << ',' << outcome.status << ',' << outcome.reason << ',';
        if (trace.work) {
            status << *trace.work;
        }
        status << ',' << trace.relative_path.generic_string() << ',' << trace.status << ',';
        if (WIFEXITED(worker.wait_status)) {
            status << WEXITSTATUS(worker.wait_status);
        }
        status << ',';
        if (WIFSIGNALED(worker.wait_status)) {
            status << WTERMSIG(worker.wait_status);
        }
        status << ',' << worker.elapsed.count() << '\n';
        write_csv(directory / "status.csv", status.str());
        return outcome.status == "failed" ? 1 : 0;
    } catch (...) {
        write_csv(directory / "status.csv", header + std::string(config.run_id) +
                  ",failed,supervisor,,,unknown,,,\n");
        throw;
    }
}
}

int main(int argc, char** argv) {
    std::optional<Config> config;
    if (argc == 2) {
        config = config_for("work");
    } else if (argc == 4 && std::string_view(argv[1]) == "--scenario") {
        config = config_for(argv[2]);
    }
    if (!config) {
        std::cerr << "usage: clab-fixture [--scenario work|completed|wall-cap|crash|memory-cap]"
                     " NEW_OUTPUT_DIRECTORY\n";
        return 2;
    }
    try {
        return run_fixture(*config, argv[argc - 1]);
    } catch (const std::exception& error) {
        std::cerr << "clab-fixture: " << error.what() << '\n';
        return 1;
    }
}
