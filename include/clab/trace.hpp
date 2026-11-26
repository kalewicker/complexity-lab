#pragma once

#include <clab/observation.hpp>

#include <filesystem>
#include <functional>
#include <memory>

namespace clab {
enum class TraceMode { strict, recover_prefix };
enum class TraceStatus { reading, complete, incomplete };

struct TraceLimits {
    std::uint32_t metadata_bytes = 64 * 1024;
    std::uint32_t chunk_bytes = 4 * 1024 * 1024;
};

struct ChunkIndexEntry {
    Stamp stamp;
    std::uint64_t header_offset{};
    std::uint16_t type{}, version{};
    std::uint32_t flags{}, payload_bytes{}, crc32c{};
};

// Called in file order only after each chunk has passed validation.
using ChunkIndexSink = std::function<void(const ChunkIndexEntry&)>;

class TraceReader final : public IObservationSource {
public:
    explicit TraceReader(const std::filesystem::path& path,
                         TraceMode mode = TraceMode::strict, TraceLimits limits = {},
                         ChunkIndexSink index_sink = {});
    ~TraceReader() override;
    [[nodiscard]] const Metadata& metadata() const noexcept override;
    [[nodiscard]] TraceStatus status() const noexcept;
    [[nodiscard]] std::uint64_t chunks_read() const noexcept;
    std::optional<ObservationRecord> next() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class TraceWriter {
public:
    // A unique staging directory adjacent to the destination holds trace.partial.
    // An unfinished writer leaves that file available for inspection and recovery.
    TraceWriter(const std::filesystem::path& path, const Metadata& metadata,
                TraceLimits limits = {});
    ~TraceWriter();
    void append(const ObservationRecord& record);
    void finish();
    [[nodiscard]] const std::filesystem::path& staging_path() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
