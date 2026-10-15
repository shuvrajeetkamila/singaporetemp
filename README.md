# singaporetemp: "Singapore 1:1 Exact" (foundations)

Status: **early foundations.** This branch contains the verified runtime core, the chunk container format, the SVY21 projection helpers, and the source-governance gate. It does not yet contain Singapore data, an Android application, or any playable content. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the full status against the specification.

## Layout

```
runtime/core/        C++17 runtime core: floating origin, .sg_chunk reader/writer, tests
pipeline/            Python tooling: SVY21 transforms, source-register validator
pipeline/manifests/  Source register (all current entries are candidates, none approved)
docs/                Architecture and status; .sg_chunk format 1.0 spec
```

## Build and test

```sh
make -C runtime/core test        # C++ core tests (needs g++ and make)
make -C runtime/core sanitize    # same tests under AddressSanitizer and UBSan

python3 -m venv .venv && . .venv/bin/activate
pip install -r pipeline/requirements.txt
cd pipeline && python -m pytest
python -m sg_pipeline.manifest validate manifests/source_register.json
```

## Release gate

The release gate (`--release`) fails while any source is not approved for build with a verified licence. This is intentional: no data enters a release build without a documented licence, date, CRS, and processing history.
