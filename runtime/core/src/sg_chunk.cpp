#include "sg/sg_chunk.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace sg::chunk {
namespace {

// Header field offsets (format 1.0).
constexpr size_t kOffMagic = 0;
constexpr size_t kOffMajor = 4;
constexpr size_t kOffMinor = 6;
constexpr size_t kOffHeaderSize = 8;
constexpr size_t kOffSectionCount = 12;
constexpr size_t kOffTableOffset = 16;
constexpr size_t kOffEntrySize = 20;
constexpr size_t kOffCellX = 24;
constexpr size_t kOffCellZ = 32;
constexpr size_t kOffCrs = 40;
constexpr size_t kOffTableCrc = 44;
constexpr size_t kOffFileSize = 48;
constexpr size_t kOffFlags = 56;
constexpr size_t kOffHeaderCrc = 60;

// Entry field offsets (40 bytes each).
constexpr size_t kEntType = 0;
constexpr size_t kEntCompression = 4;
constexpr size_t kEntOffset = 8;
constexpr size_t kEntStoredSize = 16;
constexpr size_t kEntRawSize = 24;
constexpr size_t kEntCrc = 32;
constexpr size_t kEntFlags = 36;

constexpr int64_t kMaxCellMagnitude = int64_t{1} << 40;

constexpr std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

constexpr std::array<uint32_t, 256> kCrcTable = MakeCrcTable();

void PutU16(std::vector<uint8_t>& b, size_t off, uint16_t v) {
    b[off] = static_cast<uint8_t>(v);
    b[off + 1] = static_cast<uint8_t>(v >> 8);
}

void PutU32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    for (size_t i = 0; i < 4; ++i) {
        b[off + i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

void PutU64(std::vector<uint8_t>& b, size_t off, uint64_t v) {
    for (size_t i = 0; i < 8; ++i) {
        b[off + i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

uint16_t GetU16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

uint32_t GetU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t GetU64(const uint8_t* p) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8; ++i) {
        v |= static_cast<uint64_t>(p[i]) << (8 * i);
    }
    return v;
}

int64_t GetI64(const uint8_t* p) {
    return static_cast<int64_t>(GetU64(p));
}

void PutI64(std::vector<uint8_t>& b, size_t off, int64_t v) {
    PutU64(b, off, static_cast<uint64_t>(v));
}

bool IsKnownType(uint32_t type) {
    return type >= static_cast<uint32_t>(SectionType::Metadata) &&
           type <= static_cast<uint32_t>(SectionType::SourceIds);
}

uint64_t AlignUp(uint64_t v, uint64_t alignment) {
    return (v + alignment - 1) / alignment * alignment;
}

}  // namespace

const char* ToString(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok: return "ok";
        case ErrorCode::BadArgument: return "bad argument";
        case ErrorCode::Truncated: return "truncated";
        case ErrorCode::BadMagic: return "bad magic";
        case ErrorCode::BadHeaderChecksum: return "header or table checksum mismatch";
        case ErrorCode::IncompatibleVersion: return "incompatible format version";
        case ErrorCode::BadLayout: return "invalid layout";
        case ErrorCode::Misaligned: return "misaligned offset";
        case ErrorCode::OutOfBounds: return "out-of-bounds section";
        case ErrorCode::UnsupportedCompression: return "unsupported compression";
        case ErrorCode::SectionChecksumMismatch: return "section checksum mismatch";
        case ErrorCode::DuplicateSection: return "duplicate section";
        case ErrorCode::MissingSection: return "missing section";
        case ErrorCode::TooLarge: return "chunk too large";
    }
    return "unknown";
}

uint32_t Crc32(const uint8_t* data, size_t size) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        c = kCrcTable[(c ^ data[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------------------
// ChunkWriter
// ---------------------------------------------------------------------------

ChunkWriter::ChunkWriter(int64_t cellX, int64_t cellZ, uint32_t crsEpsg)
    : cellX_(cellX), cellZ_(cellZ), crsEpsg_(crsEpsg) {}

Status ChunkWriter::AddSection(SectionType type, const std::vector<uint8_t>& payload, uint32_t flags) {
    const uint32_t raw = static_cast<uint32_t>(type);
    if (!IsKnownType(raw)) {
        return Status::Make(ErrorCode::BadArgument, "unknown section type");
    }
    if (sections_.size() >= kMaxSections) {
        return Status::Make(ErrorCode::TooLarge, "too many sections");
    }
    for (const auto& s : sections_) {
        if (s.type == raw) {
            return Status::Make(ErrorCode::DuplicateSection, "section type already added");
        }
    }
    sections_.push_back(Pending{raw, flags, payload});
    return Status{};
}

Status ChunkWriter::Finish(std::vector<uint8_t>* out) const {
    if (out == nullptr) {
        return Status::Make(ErrorCode::BadArgument, "null output");
    }
    if (cellX_ > kMaxCellMagnitude || cellX_ < -kMaxCellMagnitude ||
        cellZ_ > kMaxCellMagnitude || cellZ_ < -kMaxCellMagnitude) {
        return Status::Make(ErrorCode::BadArgument, "chunk cell out of range");
    }

    const uint64_t tableBytes = static_cast<uint64_t>(sections_.size()) * kSectionEntrySize;
    uint64_t cursor = AlignUp(kFileHeaderSize + tableBytes, kPayloadAlignment);

    struct Placed {
        uint64_t offset;
        uint64_t size;
    };
    std::vector<Placed> placed;
    placed.reserve(sections_.size());
    for (const auto& s : sections_) {
        placed.push_back(Placed{cursor, s.payload.size()});
        cursor = AlignUp(cursor + s.payload.size(), kPayloadAlignment);
    }
    const uint64_t fileSize = cursor;
    if (fileSize > kMaxFileSize) {
        return Status::Make(ErrorCode::TooLarge, "chunk exceeds format size limit");
    }

    std::vector<uint8_t> buf(static_cast<size_t>(fileSize), 0);

    // Section table, then payloads.
    for (size_t i = 0; i < sections_.size(); ++i) {
        const size_t e = kFileHeaderSize + i * kSectionEntrySize;
        const auto& s = sections_[i];
        PutU32(buf, e + kEntType, s.type);
        PutU32(buf, e + kEntCompression, static_cast<uint32_t>(Compression::None));
        PutU64(buf, e + kEntOffset, placed[i].offset);
        PutU64(buf, e + kEntStoredSize, placed[i].size);
        PutU64(buf, e + kEntRawSize, placed[i].size);
        PutU32(buf, e + kEntCrc, Crc32(s.payload.data(), s.payload.size()));
        PutU32(buf, e + kEntFlags, s.flags);
        if (!s.payload.empty()) {
            std::memcpy(buf.data() + placed[i].offset, s.payload.data(), s.payload.size());
        }
    }

    const uint32_t tableCrc = Crc32(buf.data() + kFileHeaderSize, static_cast<size_t>(tableBytes));

    std::memcpy(buf.data() + kOffMagic, kMagic, 4);
    PutU16(buf, kOffMajor, kFormatMajor);
    PutU16(buf, kOffMinor, kFormatMinor);
    PutU32(buf, kOffHeaderSize, kFileHeaderSize);
    PutU32(buf, kOffSectionCount, static_cast<uint32_t>(sections_.size()));
    PutU32(buf, kOffTableOffset, kFileHeaderSize);
    PutU32(buf, kOffEntrySize, kSectionEntrySize);
    PutI64(buf, kOffCellX, cellX_);
    PutI64(buf, kOffCellZ, cellZ_);
    PutU32(buf, kOffCrs, crsEpsg_);
    PutU32(buf, kOffTableCrc, tableCrc);
    PutU64(buf, kOffFileSize, fileSize);
    PutU32(buf, kOffFlags, 0);
    PutU32(buf, kOffHeaderCrc, Crc32(buf.data(), kOffHeaderCrc));

    *out = std::move(buf);
    return Status{};
}

// ---------------------------------------------------------------------------
// ChunkReader
// ---------------------------------------------------------------------------

Status ChunkReader::Open(const uint8_t* data, size_t size, ChunkReader* out) {
    if (out == nullptr || (data == nullptr && size > 0)) {
        return Status::Make(ErrorCode::BadArgument, "null input");
    }
    if (size < kFileHeaderSize) {
        return Status::Make(ErrorCode::Truncated, "file smaller than header");
    }
    if (static_cast<uint64_t>(size) > kMaxFileSize) {
        return Status::Make(ErrorCode::TooLarge, "file exceeds format size limit");
    }
    if (std::memcmp(data + kOffMagic, kMagic, 4) != 0) {
        return Status::Make(ErrorCode::BadMagic, "not an .sg_chunk file");
    }
    if (GetU32(data + kOffHeaderCrc) != Crc32(data, kOffHeaderCrc)) {
        return Status::Make(ErrorCode::BadHeaderChecksum, "header checksum mismatch");
    }

    const uint16_t major = GetU16(data + kOffMajor);
    const uint16_t minor = GetU16(data + kOffMinor);
    if (major != kFormatMajor) {
        return Status::Make(ErrorCode::IncompatibleVersion,
                            "format major " + std::to_string(major) + " not supported");
    }
    if (GetU32(data + kOffHeaderSize) != kFileHeaderSize ||
        GetU32(data + kOffEntrySize) != kSectionEntrySize) {
        return Status::Make(ErrorCode::BadLayout, "unexpected header or entry size");
    }

    const uint64_t declaredSize = GetU64(data + kOffFileSize);
    if (declaredSize > kMaxFileSize) {
        return Status::Make(ErrorCode::TooLarge, "declared file size exceeds limit");
    }
    if (declaredSize > size) {
        return Status::Make(ErrorCode::Truncated, "file shorter than declared size");
    }
    if (declaredSize < size) {
        return Status::Make(ErrorCode::BadLayout, "file longer than declared size");
    }

    const int64_t cellX = GetI64(data + kOffCellX);
    const int64_t cellZ = GetI64(data + kOffCellZ);
    if (cellX > kMaxCellMagnitude || cellX < -kMaxCellMagnitude ||
        cellZ > kMaxCellMagnitude || cellZ < -kMaxCellMagnitude) {
        return Status::Make(ErrorCode::BadLayout, "chunk cell out of range");
    }

    const uint32_t count = GetU32(data + kOffSectionCount);
    const uint64_t tableOffset = GetU32(data + kOffTableOffset);
    if (count > kMaxSections) {
        return Status::Make(ErrorCode::BadLayout, "section count exceeds limit");
    }
    if (tableOffset % kPayloadAlignment != 0) {
        return Status::Make(ErrorCode::Misaligned, "section table misaligned");
    }
    if (tableOffset < kFileHeaderSize) {
        return Status::Make(ErrorCode::BadLayout, "section table overlaps header");
    }
    const uint64_t tableBytes = static_cast<uint64_t>(count) * kSectionEntrySize;  // count <= 64, no overflow
    if (tableOffset > size || tableBytes > size - tableOffset) {
        return Status::Make(ErrorCode::OutOfBounds, "section table exceeds file");
    }
    const uint64_t tableEnd = tableOffset + tableBytes;

    if (GetU32(data + kOffTableCrc) !=
        Crc32(data + tableOffset, static_cast<size_t>(tableBytes))) {
        return Status::Make(ErrorCode::BadHeaderChecksum, "section table checksum mismatch");
    }

    std::vector<SectionInfo> sections;
    sections.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = data + tableOffset + static_cast<uint64_t>(i) * kSectionEntrySize;
        SectionInfo s;
        s.type = GetU32(e + kEntType);
        s.compression = GetU32(e + kEntCompression);
        s.offset = GetU64(e + kEntOffset);
        s.storedSize = GetU64(e + kEntStoredSize);
        s.rawSize = GetU64(e + kEntRawSize);
        s.crc32 = GetU32(e + kEntCrc);
        s.flags = GetU32(e + kEntFlags);

        if (s.compression != static_cast<uint32_t>(Compression::None)) {
            return Status::Make(ErrorCode::UnsupportedCompression,
                                "section " + std::to_string(s.type) + " uses compression id " +
                                    std::to_string(s.compression));
        }
        if (s.offset % kPayloadAlignment != 0) {
            return Status::Make(ErrorCode::Misaligned, "section payload misaligned");
        }
        if (s.offset < tableEnd) {
            return Status::Make(ErrorCode::BadLayout, "section payload overlaps table");
        }
        if (s.offset > size || s.storedSize > size - s.offset) {
            return Status::Make(ErrorCode::OutOfBounds, "section payload exceeds file");
        }
        if (s.rawSize != s.storedSize) {
            return Status::Make(ErrorCode::BadLayout, "raw size differs from stored size without compression");
        }
        if (!IsKnownType(s.type)) {
            if (s.flags & kSectionFlagRequired) {
                return Status::Make(ErrorCode::IncompatibleVersion,
                                    "required section type " + std::to_string(s.type) + " not understood");
            }
            // Unknown optional sections are kept in the table but ignored.
        }
        for (const auto& prev : sections) {
            if (prev.type == s.type && IsKnownType(s.type)) {
                return Status::Make(ErrorCode::DuplicateSection,
                                    "duplicate section type " + std::to_string(s.type));
            }
        }
        sections.push_back(s);
    }

    // Payloads must not overlap one another.
    std::vector<size_t> order(sections.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(),
              [&](size_t a, size_t b) { return sections[a].offset < sections[b].offset; });
    for (size_t k = 1; k < order.size(); ++k) {
        const auto& prev = sections[order[k - 1]];
        const auto& cur = sections[order[k]];
        if (cur.offset < prev.offset + prev.storedSize) {
            return Status::Make(ErrorCode::BadLayout, "section payloads overlap");
        }
    }

    out->data_ = data;
    out->size_ = size;
    out->header_.versionMajor = major;
    out->header_.versionMinor = minor;
    out->header_.cellX = cellX;
    out->header_.cellZ = cellZ;
    out->header_.crsEpsg = GetU32(data + kOffCrs);
    out->header_.fileSize = declaredSize;
    out->header_.sectionCount = count;
    out->sections_ = std::move(sections);
    return Status{};
}

bool ChunkReader::HasSection(SectionType type) const {
    const uint32_t raw = static_cast<uint32_t>(type);
    for (const auto& s : sections_) {
        if (s.type == raw) return true;
    }
    return false;
}

Status ChunkReader::GetSection(SectionType type, const uint8_t** data, size_t* size) const {
    if (data == nullptr || size == nullptr) {
        return Status::Make(ErrorCode::BadArgument, "null output");
    }
    const uint32_t raw = static_cast<uint32_t>(type);
    for (const auto& s : sections_) {
        if (s.type != raw) continue;
        const uint8_t* p = data_ + s.offset;
        if (Crc32(p, static_cast<size_t>(s.storedSize)) != s.crc32) {
            return Status::Make(ErrorCode::SectionChecksumMismatch,
                                "section " + std::to_string(raw) + " checksum mismatch");
        }
        *data = p;
        *size = static_cast<size_t>(s.storedSize);
        return Status{};
    }
    return Status::Make(ErrorCode::MissingSection, "section " + std::to_string(raw) + " not present");
}

}  // namespace sg::chunk
