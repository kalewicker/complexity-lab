#include <clab/trace.hpp>

#include <nlohmann/json.hpp>

#include <array>
#include <bit>
#include <cmath>
#include <stdlib.h>
#include <unistd.h>
#include <fstream>
#include <system_error>
#include <type_traits>
#include <utility>

namespace clab {
namespace {
using Bytes = std::vector<std::uint8_t>;
using Json = nlohmann::json;
constexpr std::size_t prefix_size = 16;
constexpr std::size_t chunk_header_size = 44;
constexpr std::uint16_t end_type = 0;
constexpr std::uint16_t snapshot_type = 1;
constexpr std::uint16_t events_type = 2;
constexpr std::uint16_t ledger_type = 3;
constexpr std::uint16_t relational_type = 4;
constexpr std::uint64_t relational_prefix = 12;
constexpr std::uint64_t entity_row_bytes = 12;
constexpr std::uint64_t relation_row_bytes = 24;
constexpr std::uint64_t port_row_bytes = 12;
constexpr std::uint32_t optional_chunk = 1;
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

template<class UInt>
void put(Bytes& bytes, UInt value) {
    static_assert(std::is_unsigned_v<UInt>);
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

template<class UInt>
UInt get(std::span<const std::uint8_t> bytes, std::size_t offset) {
    static_assert(std::is_unsigned_v<UInt>);
    UInt value = 0;
    for (std::size_t i = 0; i < sizeof(UInt); ++i) {
        value |= static_cast<UInt>(static_cast<UInt>(bytes[offset + i]) << (8 * i));
    }
    return value;
}

std::uint32_t checksum(std::span<const std::uint8_t> header,
                       std::span<const std::uint8_t> payload) {
    std::uint32_t crc = 0xffffffff;
    for (const auto part : {header, payload}) {
        for (const auto byte : part) {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit) {
                crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0U);
            }
        }
    }
    return ~crc;
}

void validate_metadata(const Metadata& meta) {
    if (meta.run_id.empty() || meta.encoding.empty() || meta.time_unit.empty()) {
        throw Error(ErrorCode::invalid, "empty metadata identifier");
    }
}

Metadata parse_metadata(const Bytes& bytes) {
    try {
        const auto obj = Json::parse(bytes, [](int depth, Json::parse_event_t, Json&) {
            if (depth > 32) {
                throw Error(ErrorCode::limit, "CLTR metadata nesting cap exceeded");
            }
            return true;
        });
        Metadata value{obj.at("run_id").get<std::string>(),
                       obj.at("encoding").get<std::string>(),
                       obj.at("time_unit").get<std::string>(),
                       obj.at("exact").get<bool>(), obj.at("fixed_sites").get<bool>()};
        validate_metadata(value);
        return value;
    } catch (const Json::exception&) {
        throw Error(ErrorCode::invalid, "invalid CLTR metadata");
    }
}

// Returns false only for an EOF-short read.
bool read_bytes(std::istream& in, std::span<std::uint8_t> bytes) {
    if (bytes.empty()) {
        return true;
    }
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (in.bad() || (in.fail() && !in.eof())) {
        throw Error(ErrorCode::io, "trace read failed");
    }
    return static_cast<std::size_t>(in.gcount()) == bytes.size();
}

void write_bytes(std::ostream& out, std::span<const std::uint8_t> bytes) {
    if (!bytes.empty()) {
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    if (!out) {
        throw Error(ErrorCode::io, "trace write failed");
    }
}

void validate_stamp(Stamp stamp, const std::optional<Stamp>& previous) {
    const auto sequence = previous ? checked_add(previous->sequence, 1) : 0;
    if (stamp.sequence != sequence || !std::isfinite(stamp.simulated_time) ||
        stamp.simulated_time < 0 || (previous && (stamp.work < previous->work ||
        stamp.simulated_time < previous->simulated_time))) {
        throw Error(ErrorCode::invalid, "invalid chunk stamp");
    }
}

void validate_events(const EventCounts& value) {
    if (value.events == 0 && (value.births || value.deaths)) {
        throw Error(ErrorCode::invalid, "birth or death in an empty event batch");
    }
}

void validate_ledger(const WorkLedger& value) {
    if (value.live_state_bytes > value.peak_state_bytes) {
        throw Error(ErrorCode::invalid, "ledger peak below live bytes");
    }
}

std::uint64_t relational_size(std::uint64_t entities, std::uint64_t relations, std::uint64_t ports) {
    auto size = checked_add(relational_prefix, checked_multiply(entities, entity_row_bytes));
    size = checked_add(size, checked_multiply(relations, relation_row_bytes));
    return checked_add(size, checked_multiply(ports, port_row_bytes));
}

RelationalSnapshot decode_relational(const Bytes& bytes) {
    if (bytes.size() < relational_prefix) {
        throw Error(ErrorCode::invalid, "short relational snapshot prefix");
    }
    const auto entity_count = get<std::uint32_t>(bytes, 0);
    const auto relation_count = get<std::uint32_t>(bytes, 4);
    const auto port_count = get<std::uint32_t>(bytes, 8);
    if (bytes.size() != relational_size(entity_count, relation_count, port_count)) {
        throw Error(ErrorCode::invalid, "invalid relational snapshot length");
    }
    std::vector<EntityRow> entities;
    std::vector<RelationRow> relations;
    std::vector<PortRow> ports;
    entities.reserve(entity_count);
    relations.reserve(relation_count);
    ports.reserve(port_count);
    std::size_t offset = relational_prefix;
    for (std::uint32_t i = 0; i < entity_count; ++i, offset += entity_row_bytes) {
        entities.push_back({get<EntityId>(bytes, offset), get<TypeId>(bytes, offset + 8)});
    }
    for (std::uint32_t i = 0; i < relation_count; ++i, offset += relation_row_bytes) {
        const auto flags = get<std::uint32_t>(bytes, offset + 20);
        if (flags > 1) {
            throw Error(ErrorCode::invalid, "unknown relational row flags");
        }
        relations.push_back({get<RelationId>(bytes, offset), get<TypeId>(bytes, offset + 8),
                             get<std::uint32_t>(bytes, offset + 12),
                             get<std::uint32_t>(bytes, offset + 16), flags == 1});
    }
    for (std::uint32_t i = 0; i < port_count; ++i, offset += port_row_bytes) {
        ports.push_back({get<EntityId>(bytes, offset), get<std::uint32_t>(bytes, offset + 8)});
    }
    return RelationalSnapshot(entities, relations, ports);
}

Bytes encode_relational(const RelationalSnapshot& snapshot, std::uint32_t chunk_limit) {
    const auto entities = snapshot.entities();
    const auto relations = snapshot.relations();
    const auto ports = snapshot.ports();
    const auto size = relational_size(entities.size(), relations.size(), ports.size());
    if (size > chunk_limit) {
        throw Error(ErrorCode::limit, "relational snapshot byte cap exceeded");
    }
    Bytes bytes;
    bytes.reserve(size);
    put(bytes, static_cast<std::uint32_t>(entities.size()));
    put(bytes, static_cast<std::uint32_t>(relations.size()));
    put(bytes, static_cast<std::uint32_t>(ports.size()));
    for (const auto& row : entities) {
        put(bytes, row.id);
        put(bytes, row.type);
    }
    for (const auto& row : relations) {
        put(bytes, row.id);
        put(bytes, row.type);
        put(bytes, row.first_port);
        put(bytes, row.port_count);
        put(bytes, std::uint32_t{row.ordered});
    }
    for (const auto& row : ports) {
        put(bytes, row.entity);
        put(bytes, row.role);
    }
    return bytes;
}
}

struct TraceReader::Impl {
    Impl(const std::filesystem::path& path, TraceMode chosen_mode, TraceLimits chosen_limits,
         ChunkIndexSink sink)
        : input(path, std::ios::binary), mode(chosen_mode), limits(chosen_limits),
          index_sink(std::move(sink)) {
        if (!input) {
            throw Error(ErrorCode::io, "could not open trace");
        }
        std::array<std::uint8_t, prefix_size> prefix{};
        if (!read_bytes(input, prefix)) {
            throw Error(ErrorCode::incomplete, "short CLTR prefix");
        }
        if (std::string(prefix.begin(), prefix.begin() + 4) != "CLTR") {
            throw Error(ErrorCode::invalid, "wrong CLTR magic");
        }
        if (get<std::uint16_t>(prefix, 4) != 1 || get<std::uint32_t>(prefix, 8) != 0x01020304) {
            throw Error(ErrorCode::unsupported, "unsupported CLTR major version or byte order");
        }
        const auto size = get<std::uint32_t>(prefix, 12);
        if (size > limits.metadata_bytes) {
            throw Error(ErrorCode::limit, "metadata byte cap exceeded");
        }
        Bytes bytes(size);
        if (!read_bytes(input, bytes)) {
            throw Error(ErrorCode::incomplete, "short CLTR metadata");
        }
        meta = parse_metadata(bytes);
        next_offset = prefix_size + std::uint64_t{size};
    }

    void accept(const ChunkIndexEntry& entry) {
        previous = entry.stamp;
        next_offset = checked_add(next_offset, chunk_header_size + std::uint64_t{entry.payload_bytes});
        ++chunks;
        if (index_sink) {
            index_sink(entry);
        }
    }

    std::optional<ObservationRecord> short_chunk() {
        status = TraceStatus::incomplete;
        if (mode == TraceMode::strict) {
            throw Error(ErrorCode::incomplete, "trace ended before a complete end chunk");
        }
        return std::nullopt;
    }

    std::ifstream input;
    TraceMode mode;
    TraceLimits limits;
    Metadata meta;
    TraceStatus status{TraceStatus::reading};
    std::optional<Stamp> previous;
    bool failed{};
    std::uint64_t chunks{}, next_offset{};
    ChunkIndexSink index_sink;
};

TraceReader::TraceReader(const std::filesystem::path& path, TraceMode mode, TraceLimits limits,
                         ChunkIndexSink index_sink)
    : impl_(std::make_unique<Impl>(path, mode, limits, std::move(index_sink))) {}
TraceReader::~TraceReader() = default;

const Metadata& TraceReader::metadata() const noexcept { return impl_->meta; }
TraceStatus TraceReader::status() const noexcept { return impl_->status; }
std::uint64_t TraceReader::chunks_read() const noexcept { return impl_->chunks; }

std::optional<ObservationRecord> TraceReader::next() {
    auto& reader = *impl_;
    if (reader.failed) {
        throw Error(ErrorCode::invalid, "trace reader stopped after an error");
    }
    if (reader.status != TraceStatus::reading) {
        return std::nullopt;
    }

    try {
        for (;;) {
            std::array<std::uint8_t, chunk_header_size> header{};
            if (!read_bytes(reader.input, header)) {
                return reader.short_chunk();
            }
            const auto type = get<std::uint16_t>(header, 0);
            const auto version = get<std::uint16_t>(header, 2);
            const auto flags = get<std::uint32_t>(header, 4);
            const Stamp stamp{get<std::uint64_t>(header, 8),
                              std::bit_cast<double>(get<std::uint64_t>(header, 16)),
                              get<std::uint64_t>(header, 24)};
            const auto size = get<std::uint32_t>(header, 36);
            const auto raw_size = get<std::uint32_t>(header, 32);
            validate_stamp(stamp, reader.previous);
            if (size > reader.limits.chunk_bytes || raw_size > reader.limits.chunk_bytes) {
                throw Error(ErrorCode::limit, "chunk byte cap exceeded");
            }
            if (flags & ~optional_chunk || raw_size != size) {
                throw Error(ErrorCode::unsupported, "unsupported chunk flags or compression");
            }

            Bytes bytes(size);
            if (!read_bytes(reader.input, bytes)) {
                return reader.short_chunk();
            }
            const auto crc = get<std::uint32_t>(header, 40);
            if (checksum(std::span(header).first(40), bytes) != crc) {
                throw Error(ErrorCode::invalid, "chunk checksum mismatch");
            }
            const ChunkIndexEntry entry{stamp, reader.next_offset, type, version, flags, size, crc};
            if (type > relational_type || version != 1) {
                if (type != end_type && (flags & optional_chunk)) {
                    reader.accept(entry);
                    continue;
                }
                throw Error(ErrorCode::unsupported, "unknown required chunk or version");
            }

            switch (type) {
            case end_type:
                if (size != 0 || flags != 0 || (stamp.sequence == 0 &&
                    (stamp.work != 0 || stamp.simulated_time != 0))) {
                    throw Error(ErrorCode::invalid, "invalid end chunk");
                }
                if (reader.input.peek() != std::char_traits<char>::eof()) {
                    throw Error(ErrorCode::invalid, "trailing bytes after end chunk");
                }
                if (reader.input.bad()) {
                    throw Error(ErrorCode::io, "end of trace check failed");
                }
                reader.accept(entry);
                reader.status = TraceStatus::complete;
                return std::nullopt;
            case snapshot_type: {
                if (size % sizeof(TypeId) != 0) {
                    throw Error(ErrorCode::invalid, "invalid categorical snapshot length");
                }
                std::vector<TypeId> symbols;
                symbols.reserve(size / sizeof(TypeId));
                for (std::size_t offset = 0; offset < size; offset += sizeof(TypeId)) {
                    symbols.push_back(get<TypeId>(bytes, offset));
                }
                ObservationRecord record{stamp, Snapshot(symbols)};
                reader.accept(entry);
                return record;
            }
            case events_type: {
                if (size != 24) {
                    throw Error(ErrorCode::invalid, "invalid event batch length");
                }
                EventCounts events{get<std::uint64_t>(bytes, 0), get<std::uint64_t>(bytes, 8),
                                   get<std::uint64_t>(bytes, 16)};
                validate_events(events);
                reader.accept(entry);
                return ObservationRecord{stamp, events};
            }
            case ledger_type: {
                if (size != 48) {
                    throw Error(ErrorCode::invalid, "invalid ledger length");
                }
                WorkLedger ledger{get<std::uint64_t>(bytes, 0), get<std::uint64_t>(bytes, 8),
                                  get<std::uint64_t>(bytes, 16), get<std::uint64_t>(bytes, 24),
                                  get<std::uint64_t>(bytes, 32), get<std::uint64_t>(bytes, 40)};
                validate_ledger(ledger);
                reader.accept(entry);
                return ObservationRecord{stamp, ledger};
            }
            case relational_type: {
                ObservationRecord record{stamp, decode_relational(bytes)};
                reader.accept(entry);
                return record;
            }
            }
        }
    } catch (...) {
        reader.failed = true;
        throw;
    }
}

struct TraceWriter::Impl {
    Impl(const std::filesystem::path& path, const Metadata& meta, TraceLimits chosen_limits)
        : destination(path), limits(chosen_limits) {
        validate_metadata(meta);
        std::string json;
        try {
            json = Json{{"run_id", meta.run_id}, {"encoding", meta.encoding},
                        {"time_unit", meta.time_unit}, {"exact", meta.exact},
                        {"fixed_sites", meta.fixed_sites}}.dump();
        } catch (const Json::exception&) {
            throw Error(ErrorCode::invalid, "metadata must be UTF-8");
        }
        if (json.size() > limits.metadata_bytes) {
            throw Error(ErrorCode::limit, "metadata byte cap exceeded");
        }
        auto pattern = (path.parent_path() / ".cltr-XXXXXX").string();
        if (!::mkdtemp(pattern.data())) {
            throw Error(ErrorCode::io, "could not create trace staging directory");
        }
        staging = std::filesystem::path(pattern) / "trace.partial";
        output.open(staging, std::ios::binary);
        Bytes prefix{'C', 'L', 'T', 'R'};
        put(prefix, std::uint16_t{1});
        put(prefix, std::uint16_t{0});
        put(prefix, std::uint32_t{0x01020304});
        put(prefix, static_cast<std::uint32_t>(json.size()));
        write_bytes(output, prefix);
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        flush();
    }

    void flush() {
        output.flush();
        if (!output) {
            throw Error(ErrorCode::io, "could not flush staged trace");
        }
    }

    void chunk(std::uint16_t type, Stamp stamp, const Bytes& payload) {
        validate_stamp(stamp, previous);
        if (payload.size() > limits.chunk_bytes) {
            throw Error(ErrorCode::limit, "chunk byte cap exceeded");
        }
        Bytes header;
        put(header, type);
        put(header, std::uint16_t{1});
        put(header, std::uint32_t{0});
        put(header, stamp.sequence);
        put(header, std::bit_cast<std::uint64_t>(stamp.simulated_time));
        put(header, stamp.work);
        put(header, static_cast<std::uint32_t>(payload.size()));
        put(header, static_cast<std::uint32_t>(payload.size()));
        put(header, checksum(header, payload));
        if (!output) {
            throw Error(ErrorCode::io, "staged trace writer is in a failed state");
        }
        write_bytes(output, header);
        write_bytes(output, payload);
        flush();
        previous = stamp;
    }

    std::filesystem::path destination, staging;
    TraceLimits limits;
    std::ofstream output;
    std::optional<Stamp> previous;
    bool sealed{}, published{};
};

TraceWriter::TraceWriter(const std::filesystem::path& path, const Metadata& meta, TraceLimits limits)
    : impl_(std::make_unique<Impl>(path, meta, limits)) {}
TraceWriter::~TraceWriter() = default;

const std::filesystem::path& TraceWriter::staging_path() const noexcept { return impl_->staging; }

void TraceWriter::append(const ObservationRecord& record) {
    auto& writer = *impl_;
    if (writer.sealed) {
        throw Error(ErrorCode::invalid, "trace already sealed");
    }
    Bytes bytes;
    std::uint16_t type;
    if (const auto* snap = std::get_if<Snapshot>(&record.payload)) {
        type = snapshot_type;
        if (snap->symbols().size() > writer.limits.chunk_bytes / sizeof(TypeId)) {
            throw Error(ErrorCode::limit, "snapshot byte cap exceeded");
        }
        bytes.reserve(snap->symbols().size_bytes());
        for (const auto symbol : snap->symbols()) {
            put(bytes, symbol);
        }
    } else if (const auto* snapshot = std::get_if<RelationalSnapshot>(&record.payload)) {
        type = relational_type;
        bytes = encode_relational(*snapshot, writer.limits.chunk_bytes);
    } else if (const auto* events = std::get_if<EventCounts>(&record.payload)) {
        type = events_type;
        validate_events(*events);
        put(bytes, events->events);
        put(bytes, events->births);
        put(bytes, events->deaths);
    } else {
        type = ledger_type;
        const auto& ledger = std::get<WorkLedger>(record.payload);
        validate_ledger(ledger);
        for (const auto count : {ledger.logical_updates, ledger.reads, ledger.writes,
                                  ledger.rng_draws, ledger.live_state_bytes, ledger.peak_state_bytes}) {
            put(bytes, count);
        }
    }
    writer.chunk(type, record.stamp, bytes);
}

void TraceWriter::finish() {
    auto& writer = *impl_;
    if (writer.published) {
        return;
    }
    if (!writer.sealed) {
        auto stamp = writer.previous.value_or(Stamp{});
        if (writer.previous) {
            stamp.sequence = checked_add(stamp.sequence, 1);
        }
        writer.chunk(end_type, stamp, {});
        writer.sealed = true;
        writer.output.close();
    }
    if (!writer.output) {
        throw Error(ErrorCode::io, "could not close staged trace");
    }
    std::error_code error;
    // Same-filesystem atomic publication with no replacement of an existing trace.
    std::filesystem::create_hard_link(writer.staging, writer.destination, error);
    if (error) {
        throw Error(ErrorCode::io, "could not publish trace: " + error.message());
    }
    writer.published = true;
    std::filesystem::remove(writer.staging, error);
    std::filesystem::remove(writer.staging.parent_path(), error);
}
}
