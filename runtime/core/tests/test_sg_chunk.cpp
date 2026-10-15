#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "sg/sg_chunk.hpp"
#include "test_common.hpp"

using namespace sg::chunk;

namespace {

std::vector<uint8_t> Bytes(const char* s) {
    return std::vector<uint8_t>(s, s + std::strlen(s));
}

std::vector<uint8_t> MakeSampleChunk() {
    ChunkWriter w(12, -3, 3414);
    SG_CHECK(w.AddSection(SectionType::Metadata, Bytes("manifest:test-fixture")).ok());
    std::vector<uint8_t> geometry(1000);
    for (size_t i = 0; i < geometry.size(); ++i) geometry[i] = static_cast<uint8_t>(i * 7);
    SG_CHECK(w.AddSection(SectionType::Geometry, geometry).ok());
    SG_CHECK(w.AddSection(SectionType::Collision, Bytes("collision-page")).ok());
    std::vector<uint8_t> out;
    SG_CHECK(w.Finish(&out).ok());
    return out;
}

// Recomputes section and table checksums so fuzzing can reach deeper validation.
void FixChecksums(std::vector<uint8_t>& b) {
    if (b.size() < kFileHeaderSize) return;
    auto u32 = [&](size_t off) {
        return static_cast<uint32_t>(b[off]) | (static_cast<uint32_t>(b[off + 1]) << 8) |
               (static_cast<uint32_t>(b[off + 2]) << 16) | (static_cast<uint32_t>(b[off + 3]) << 24);
    };
    auto u64 = [&](size_t off) {
        uint64_t v = 0;
        for (size_t i = 0; i < 8; ++i) v |= static_cast<uint64_t>(b[off + i]) << (8 * i);
        return v;
    };
    auto put32 = [&](size_t off, uint32_t v) {
        for (size_t i = 0; i < 4; ++i) b[off + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    const uint32_t count = u32(12);
    const uint64_t tableOff = u32(16);
    if (count <= kMaxSections && tableOff <= b.size() && count * kSectionEntrySize <= b.size() - tableOff) {
        for (uint32_t i = 0; i < count; ++i) {
            const size_t e = tableOff + i * kSectionEntrySize;
            const uint64_t off = u64(e + 8);
            const uint64_t sz = u64(e + 16);
            if (off <= b.size() && sz <= b.size() - off) {
                put32(e + 32, Crc32(b.data() + off, static_cast<size_t>(sz)));
            }
        }
        put32(44, Crc32(b.data() + tableOff, count * kSectionEntrySize));
    }
    put32(60, Crc32(b.data(), 60));
}

void TestCrcVectors() {
    const char* check = "123456789";
    SG_CHECK(Crc32(reinterpret_cast<const uint8_t*>(check), 9) == 0xCBF43926u);
    SG_CHECK(Crc32(nullptr, 0) == 0u);
}

void TestRoundTrip() {
    auto bytes = MakeSampleChunk();
    ChunkReader r;
    Status s = ChunkReader::Open(bytes.data(), bytes.size(), &r);
    SG_CHECK(s.ok());
    if (!s.ok()) {
        std::fprintf(stderr, "open failed: %s\n", s.message.c_str());
        return;
    }
    SG_CHECK(r.Header().cellX == 12 && r.Header().cellZ == -3);
    SG_CHECK(r.Header().crsEpsg == 3414);
    SG_CHECK(r.Header().versionMajor == kFormatMajor);
    SG_CHECK(r.Header().sectionCount == 3);
    SG_CHECK(r.HasSection(SectionType::Metadata));
    SG_CHECK(!r.HasSection(SectionType::Navigation));

    for (const auto& sec : r.Sections()) {
        SG_CHECK(sec.offset % kPayloadAlignment == 0);
    }

    const uint8_t* p = nullptr;
    size_t n = 0;
    SG_CHECK(r.GetSection(SectionType::Geometry, &p, &n).ok());
    SG_CHECK(n == 1000);
    bool match = true;
    for (size_t i = 0; i < n; ++i) match = match && (p[i] == static_cast<uint8_t>(i * 7));
    SG_CHECK(match);

    SG_CHECK(r.GetSection(SectionType::Navigation, &p, &n).code == ErrorCode::MissingSection);
}

void TestEmptyChunk() {
    ChunkWriter w(0, 0, 3414);
    std::vector<uint8_t> bytes;
    SG_CHECK(w.Finish(&bytes).ok());
    ChunkReader r;
    SG_CHECK(ChunkReader::Open(bytes.data(), bytes.size(), &r).ok());
    SG_CHECK(r.Header().sectionCount == 0);
}

void TestWriterRejections() {
    ChunkWriter w(0, 0, 3414);
    SG_CHECK(w.AddSection(SectionType::Geometry, Bytes("a")).ok());
    SG_CHECK(w.AddSection(SectionType::Geometry, Bytes("b")).code == ErrorCode::DuplicateSection);
    SG_CHECK(w.AddSection(static_cast<SectionType>(99), Bytes("c")).code == ErrorCode::BadArgument);
    ChunkWriter bad(int64_t{1} << 50, 0, 3414);
    std::vector<uint8_t> out;
    SG_CHECK(bad.Finish(&out).code == ErrorCode::BadArgument);
}

void TestCorruptionDiagnostics() {
    const auto good = MakeSampleChunk();
    ChunkReader r;

    {  // Truncated file.
        std::vector<uint8_t> b(good.begin(), good.end() - 1);
        SG_CHECK(ChunkReader::Open(b.data(), b.size(), &r).code == ErrorCode::Truncated);
    }
    {  // Smaller than the header.
        std::vector<uint8_t> b(10, 0);
        SG_CHECK(ChunkReader::Open(b.data(), b.size(), &r).code == ErrorCode::Truncated);
    }
    {  // Bad magic.
        auto b = good;
        b[0] = 'X';
        SG_CHECK(ChunkReader::Open(b.data(), b.size(), &r).code == ErrorCode::BadMagic);
    }
    {  // Header bit flip is caught by the header checksum.
        auto b = good;
        b[20] ^= 0x01;
        SG_CHECK(ChunkReader::Open(b.data(), b.size(), &r).code == ErrorCode::BadHeaderChecksum);
    }
    {  // Table bit flip is caught by the table checksum.
        auto b = good;
        b[kFileHeaderSize + 9] ^= 0x40;
        SG_CHECK(ChunkReader::Open(b.data(), b.size(), &r).code == ErrorCode::BadHeaderChecksum);
    }
    {  // Payload corruption is detected when that section is read, not at open.
        auto b = good;
        ChunkReader ok;
        SG_CHECK(ChunkReader::Open(good.data(), good.size(), &ok).ok());
        const SectionInfo geo = [&]() {
            for (const auto& s : ok.Sections()) {
                if (s.type == static_cast<uint32_t>(SectionType::Geometry)) return s;
            }
            return SectionInfo{};
        }();
        b[static_cast<size_t>(geo.offset) + 3] ^= 0xFF;
        ChunkReader r2;
        SG_CHECK(ChunkReader::Open(b.data(), b.size(), &r2).ok());
        const uint8_t* p = nullptr;
        size_t n = 0;
        SG_CHECK(r2.GetSection(SectionType::Metadata, &p, &n).ok());  // untouched section still reads
        SG_CHECK(r2.GetSection(SectionType::Geometry, &p, &n).code == ErrorCode::SectionChecksumMismatch);
    }
    {  // Incompatible major version, with checksums repaired so only the version is wrong.
        auto b = good;
        b[4] = 2;
        FixChecksums(b);
        SG_CHECK(ChunkReader::Open(b.data(), b.size(), &r).code == ErrorCode::IncompatibleVersion);
    }
    {  // Unsupported compression id in the table.
        auto b = good;
        b[kFileHeaderSize + 4] = 1;  // entry 0 compression = zstd (not implemented)
        FixChecksums(b);
        SG_CHECK(ChunkReader::Open(b.data(), b.size(), &r).code == ErrorCode::UnsupportedCompression);
    }
    {  // Misaligned payload offset.
        auto b = good;
        b[kFileHeaderSize + 8] = static_cast<uint8_t>(b[kFileHeaderSize + 8] + 1);
        FixChecksums(b);
        SG_CHECK(ChunkReader::Open(b.data(), b.size(), &r).code == ErrorCode::Misaligned);
    }
    {  // Payload offset far beyond file end must be rejected without wraparound.
        auto b = good;
        for (int i = 0; i < 8; ++i) b[kFileHeaderSize + 8 + i] = 0xF0;
        FixChecksums(b);
        const auto code = ChunkReader::Open(b.data(), b.size(), &r).code;
        SG_CHECK(code == ErrorCode::OutOfBounds || code == ErrorCode::Misaligned);
    }
    {  // Unknown required section type is rejected.
        ChunkWriter w(0, 0, 3414);
        SG_CHECK(w.AddSection(SectionType::Geometry, Bytes("g"), kSectionFlagRequired).ok());
        std::vector<uint8_t> b;
        SG_CHECK(w.Finish(&b).ok());
        b[kFileHeaderSize] = 200;  // unknown type
        FixChecksums(b);
        SG_CHECK(ChunkReader::Open(b.data(), b.size(), &r).code == ErrorCode::IncompatibleVersion);
    }
    {  // Unknown optional section type is skipped.
        ChunkWriter w(0, 0, 3414);
        SG_CHECK(w.AddSection(SectionType::Geometry, Bytes("g"), 0).ok());
        std::vector<uint8_t> b;
        SG_CHECK(w.Finish(&b).ok());
        b[kFileHeaderSize] = 200;
        FixChecksums(b);
        SG_CHECK(ChunkReader::Open(b.data(), b.size(), &r).ok());
    }
    {  // Null arguments are rejected.
        SG_CHECK(ChunkReader::Open(nullptr, 100, &r).code == ErrorCode::BadArgument);
    }
}

}  // namespace

void RunChunkTests() {
    TestCrcVectors();
    TestRoundTrip();
    TestEmptyChunk();
    TestWriterRejections();
    TestCorruptionDiagnostics();
}

// Mutation fuzzing: random byte edits, truncations, extensions, and field overwrites.
// Half the iterations repair checksums so the layout validators are exercised too.
// Run under AddressSanitizer and UBSan (see Makefile target `sanitize`).
void RunChunkFuzzTests(int iterations) {
    const auto seed = MakeSampleChunk();
    std::mt19937_64 rng(0xC0FFEEu);
    std::uniform_int_distribution<int> pick(0, 9);
    int openedOk = 0;
    int readOk = 0;

    for (int it = 0; it < iterations; ++it) {
        std::vector<uint8_t> b = seed;
        const int mutations = 1 + static_cast<int>(rng() % 4);
        for (int m = 0; m < mutations; ++m) {
            switch (pick(rng)) {
                case 0:  // flip a random bit
                    if (!b.empty()) b[rng() % b.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
                    break;
                case 1:  // random byte
                    if (!b.empty()) b[rng() % b.size()] = static_cast<uint8_t>(rng());
                    break;
                case 2:  // truncate
                    b.resize(rng() % (b.size() + 1));
                    break;
                case 3:  // extend with junk
                    for (size_t k = rng() % 64; k > 0; --k) b.push_back(static_cast<uint8_t>(rng()));
                    break;
                case 4: {  // overwrite a u64 with an extreme value
                    if (b.size() >= 8) {
                        const size_t off = rng() % (b.size() - 7);
                        const uint64_t v = (rng() & 1) ? UINT64_MAX : (rng() & 1 ? 0 : rng());
                        for (size_t i = 0; i < 8; ++i) b[off + i] = static_cast<uint8_t>(v >> (8 * i));
                    }
                    break;
                }
                case 5: {  // overwrite a u32 field with an extreme value
                    if (b.size() >= 4) {
                        const size_t off = rng() % (b.size() - 3);
                        const uint32_t v = (rng() & 1) ? UINT32_MAX : static_cast<uint32_t>(rng());
                        for (size_t i = 0; i < 4; ++i) b[off + i] = static_cast<uint8_t>(v >> (8 * i));
                    }
                    break;
                }
                default:  // header or table region only
                    if (b.size() > kFileHeaderSize) {
                        b[rng() % (b.size() < 256 ? b.size() : 256)] ^= 0xFF;
                    }
                    break;
            }
        }
        if (rng() & 1) FixChecksums(b);

        ChunkReader r;
        Status s = ChunkReader::Open(b.data(), b.size(), &r);
        if (!s.ok()) continue;
        ++openedOk;
        for (const auto type : {SectionType::Metadata, SectionType::Geometry, SectionType::Collision,
                                SectionType::Navigation}) {
            const uint8_t* p = nullptr;
            size_t n = 0;
            Status rs = r.GetSection(type, &p, &n);
            if (rs.ok()) {
                ++readOk;
                // Touch every byte so sanitizers see any out-of-bounds read.
                volatile uint8_t sink = 0;
                for (size_t i = 0; i < n; ++i) sink = static_cast<uint8_t>(sink ^ p[i]);
                (void)sink;
            }
        }
    }
    std::printf("fuzz: %d iterations, %d opened, %d section reads ok\n", iterations, openedOk, readOk);
    // The unmutated seed must always pass; this guards the fuzzer against a broken writer.
    ChunkReader r;
    SG_CHECK(ChunkReader::Open(seed.data(), seed.size(), &r).ok());
}
