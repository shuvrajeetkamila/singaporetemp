// .sg_chunk container, format version 1.0. Full layout: docs/SG_CHUNK_FORMAT.md
//
// All integers are little-endian and are encoded/decoded byte by byte, so the
// format does not depend on host endianness or struct padding.
//
// Reader guarantees:
//   * every offset and length is bounds-checked with overflow-safe arithmetic;
//   * the header and section table are checksummed (CRC-32/IEEE);
//   * each section payload is checksummed and verified when it is read;
//   * malformed input returns a Status with a diagnostic and never crashes.
//
// Only compression id 0 (none) is implemented in format 1.0. Other ids are
// rejected with UnsupportedCompression until a codec and its tests are added.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sg::chunk {

inline constexpr char kMagic[4] = {'S', 'G', 'C', 'H'};
inline constexpr uint16_t kFormatMajor = 1;
inline constexpr uint16_t kFormatMinor = 0;
inline constexpr uint32_t kFileHeaderSize = 64;
inline constexpr uint32_t kSectionEntrySize = 40;
inline constexpr uint64_t kPayloadAlignment = 16;
inline constexpr uint32_t kMaxSections = 64;
inline constexpr uint64_t kMaxFileSize = uint64_t{1} << 31;  // 2 GiB per chunk

// Section flag bit 0: reader must understand this section type or reject the chunk.
inline constexpr uint32_t kSectionFlagRequired = 1u << 0;

enum class SectionType : uint32_t {
    Metadata = 1,       // schema, build id, source-manifest reference, seeds
    Bounds = 2,         // AABBs and bounding spheres per LOD
    Geometry = 3,       // vertex and index pages
    Collision = 4,      // collision shapes and references
    Lod = 5,            // LOD graph and geomorph parameters
    Navigation = 6,     // drivable and walkable graph
    MaterialMask = 7,   // ground-type and wetness masks
    SourceIds = 8,      // per-feature source identifiers and confidence
};

enum class Compression : uint32_t {
    None = 0,
};

enum class ErrorCode : uint8_t {
    Ok,
    BadArgument,
    Truncated,
    BadMagic,
    BadHeaderChecksum,
    IncompatibleVersion,
    BadLayout,
    Misaligned,
    OutOfBounds,
    UnsupportedCompression,
    SectionChecksumMismatch,
    DuplicateSection,
    MissingSection,
    TooLarge,
};

const char* ToString(ErrorCode code);

struct Status {
    ErrorCode code = ErrorCode::Ok;
    std::string message;

    bool ok() const { return code == ErrorCode::Ok; }
    static Status Make(ErrorCode code, std::string message) { return Status{code, std::move(message)}; }
};

struct SectionInfo {
    uint32_t type = 0;
    uint32_t compression = 0;
    uint64_t offset = 0;
    uint64_t storedSize = 0;
    uint64_t rawSize = 0;
    uint32_t crc32 = 0;
    uint32_t flags = 0;
};

struct ChunkHeaderInfo {
    uint16_t versionMajor = 0;
    uint16_t versionMinor = 0;
    int64_t cellX = 0;
    int64_t cellZ = 0;
    uint32_t crsEpsg = 0;
    uint64_t fileSize = 0;
    uint32_t sectionCount = 0;
};

// CRC-32/IEEE (reflected, polynomial 0xEDB88320). Check value for "123456789" is 0xCBF43926.
uint32_t Crc32(const uint8_t* data, size_t size);

class ChunkWriter {
public:
    ChunkWriter(int64_t cellX, int64_t cellZ, uint32_t crsEpsg);

    // Adds an uncompressed section. Known section types may appear only once.
    Status AddSection(SectionType type, const std::vector<uint8_t>& payload, uint32_t flags = 0);

    // Serializes header, section table, and 16-byte-aligned payloads.
    Status Finish(std::vector<uint8_t>* out) const;

private:
    struct Pending {
        uint32_t type;
        uint32_t flags;
        std::vector<uint8_t> payload;
    };
    int64_t cellX_;
    int64_t cellZ_;
    uint32_t crsEpsg_;
    std::vector<Pending> sections_;
};

// Read-only view over a chunk. The caller owns the bytes and must keep them alive
// (the same contract as a memory-mapped file).
class ChunkReader {
public:
    // Validates the header, section table, and layout. Section payload CRCs are checked in GetSection.
    static Status Open(const uint8_t* data, size_t size, ChunkReader* out);

    const ChunkHeaderInfo& Header() const { return header_; }
    const std::vector<SectionInfo>& Sections() const { return sections_; }
    bool HasSection(SectionType type) const;

    // Returns a pointer to the raw payload after verifying its CRC.
    Status GetSection(SectionType type, const uint8_t** data, size_t* size) const;

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    ChunkHeaderInfo header_;
    std::vector<SectionInfo> sections_;
};

}  // namespace sg::chunk
