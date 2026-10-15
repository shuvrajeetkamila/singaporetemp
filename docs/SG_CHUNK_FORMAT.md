# `.sg_chunk` format, version 1.0

Status: implemented in `runtime/core/src/sg_chunk.cpp` (reader and writer, CRC-32/IEEE).
Compression: only id `0` (none). Other ids are rejected with `UnsupportedCompression`.

## Principles

- **Little-endian, explicit.** Every integer is encoded byte by byte, so layout does not depend on host endianness or struct padding.
- **Checksummed.** The header is covered by `headerCrc32`, the section table by `tableCrc32`, and each payload by its own CRC.
- **Aligned.** Section table and payloads start on 16-byte boundaries.
- **Independently readable.** Each section is a separate payload, so a reader can load one section without touching the rest.
- **Memory-map ready.** Uncompressed payloads can be used in place. Compressed pages will be decompressed on demand, one page at a time. A compressed archive is never treated as directly mappable.
- **Fails safely.** Malformed input returns a `Status` code and message. The reader never crashes or reads out of bounds.

## File header (64 bytes)

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | magic | `SGCH` |
| 4 | 2 | versionMajor | `1`. Other majors are rejected. |
| 6 | 2 | versionMinor | `0`. Newer minors are accepted, and unknown optional sections are ignored. |
| 8 | 4 | headerSize | `64` |
| 12 | 4 | sectionCount | `0..64` |
| 16 | 4 | sectionTableOffset | 16-byte aligned, at least 64 |
| 20 | 4 | sectionEntrySize | `40` |
| 24 | 8 | cellX (i64) | Macro-cell index, |value| ≤ 2^40 |
| 32 | 8 | cellZ (i64) | Macro-cell index |
| 40 | 4 | crsEpsg | `3414` for SVY21 |
| 44 | 4 | tableCrc32 | CRC-32 over the section table bytes |
| 48 | 8 | fileSize | Must equal the actual file size |
| 56 | 4 | flags | Reserved, `0` |
| 60 | 4 | headerCrc32 | CRC-32 over bytes 0..59 |

## Section table

`sectionCount` entries of 40 bytes each, starting at `sectionTableOffset`.

| Offset | Size | Field | Notes |
|---:|---:|---|---|
| 0 | 4 | type | See the section types below |
| 4 | 4 | compression | `0` = none |
| 8 | 8 | offset | 16-byte aligned, after the table |
| 16 | 8 | storedSize | Bytes in the file |
| 24 | 8 | rawSize | Must equal storedSize when compression is `0` |
| 32 | 4 | crc32 | CRC-32 over the stored bytes |
| 36 | 4 | flags | Bit 0: required. The reader rejects unknown required types. |

Section types: `1` Metadata, `2` Bounds, `3` Geometry, `4` Collision, `5` LOD, `6` Navigation, `7` MaterialMask, `8` SourceIds. Known types may appear at most once. Unknown types with the required flag are rejected. Unknown types without it are skipped.

## Validation order (reader)

1. Size is at least 64 bytes and at most 2 GiB.
2. Magic matches.
3. Header CRC matches.
4. Major version is 1. Header and entry sizes match.
5. Declared file size equals the actual size.
6. Cell indices are within range.
7. Section count is at most 64. Table offset is aligned and in bounds. Table CRC matches.
8. Each entry: compression is supported, offset is aligned, payload is after the table and in bounds (overflow-safe), raw size equals stored size, known types are unique, payloads do not overlap.
9. On `GetSection`: payload CRC matches.

## Test coverage

- CRC-32 check vector (`"123456789"` → `0xCBF43926`).
- Round-trip of header fields and payloads.
- One test per rejection class: truncation, bad magic, header/table checksum, incompatible version, unsupported compression, misalignment, out-of-bounds offsets, unknown required and optional types, and payload corruption detected at read time.
- Mutation fuzzing (20k iterations in the default run, 100k under ASan/UBSan), with half of the iterations repairing checksums so the layout validators are exercised.

## Not yet in format 1.0

- Compression codecs and page tables for independently decompressed pages.
- Platform memory-mapping wrapper (the reader already takes a pointer and size, so `mmap` plugs in without changes).
- Per-section schemas for geometry, collision, navigation, and material masks. The pipeline defines these once the source data is chosen.
