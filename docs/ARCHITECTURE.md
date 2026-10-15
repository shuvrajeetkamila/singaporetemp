# Architecture and status: Project "Singapore 1:1 Exact"

This document maps the master build specification to what exists in the repository, what is blocked, and why. It is the honest status record, not a plan for the finished product.

## Milestone 0 (this branch): foundations that can be verified here

| Area | What is built | How it is verified |
|---|---|---|
| Floating origin | `GlobalPosition` (512 m cells, floor-based normalization, checked int64 arithmetic), `FloatingOriginManager` (origin epoch, shift event, physics bulk-shift hook) | `runtime/core`: 716k checks, including traversal at walking speed (1.4 m/s) and vehicle speed (30 m/s) across cell boundaries |
| Chunk container | `.sg_chunk` format 1.0 reader and writer, checksummed, aligned, little-endian | `runtime/core`: rejection tests, payload-corruption tests, 100k-iteration mutation fuzzing under ASan and UBSan |
| SVY21 projection | pyproj-based EPSG:3414 transforms, cell-index helpers | `pipeline`: origin matches the SLA definition to < 1 µm; round-trip tests; placement plausibility |
| Source governance | Source register schema, validator, and release gate | `pipeline`: schema tests; the release gate blocks every current candidate |

Commands:

```
make -C runtime/core test          # C++ core tests
make -C runtime/core sanitize      # same tests under ASan + UBSan, 100k fuzz iterations
cd pipeline && python -m pytest    # SVY21 and manifest tests (needs requirements.txt)
python -m sg_pipeline.manifest validate pipeline/manifests/source_register.json
```

### Measured results (this sandbox, x86-64, g++ 12.2, -O2)

- Float32 render error: below 1.6e-5 m for entities inside one cell; at most 2.44e-4 m across the ±4 km active window.
- Walking traversal (1.4 m/s, 40 min at 60 Hz, 6 origin shifts): max static-entity error 2.2e-5 m; max relative-transform error 3.1e-5 m.
- Vehicle traversal (30 m/s, 10 min, 35 origin shifts, 9,884 in-window samples): max static error 1.2e-4 m; max relative error 1.2e-4 m.
- Fuzzing: 100k mutated chunks under ASan and UBSan, no memory or undefined-behavior reports.

These are numerical results for this build on this host. They are not device performance results.

## Specification status by section

| Spec section | Status | Blocker or note |
|---|---|---|
| 0. Mission and standards | Governance scaffolding only | No Android app exists yet |
| 1. Geospatial pipeline (Rust) | **Not started.** Only SVY21 transforms and the register exist. | Rust toolchain is not installed, and `rustup` needs hosts outside the allowlist. Government data sources (SLA, LTA, data.gov.sg, URA) are not reachable from the sandbox. |
| 1A. Terrain | Not started | No DEM is chosen. The vertical reference is not yet defined. |
| 1B. Roads and rail | Not started | No licensed alignment data. |
| 1C. Buildings | Not started | No licensed footprints or heights. |
| 1D. Coastline and water | Not started | No dated coastline or bathymetry source selected. |
| 2. Zoning and landmarks | Not started | Needs licensed sources and reviewed reference imagery. |
| 3. Atmosphere, weather, water | Not started | Renderer not started. |
| 4. Floating origin | **Core implemented and tested.** Physics integration (Jolt) and engine hookup are not started. | Needs the engine and physics system to exist. |
| 5. Traffic and pedestrians | Not started | Needs the road graph (§1B). |
| 6. Streaming and `.sg_chunk` | **Container format 1.0 implemented.** Compression codecs, mmap wrapper, and streaming scheduler are not started. | |
| 6B–6C. Android and Vulkan | Not started | No Android SDK or NDK, and no target devices in the sandbox. |
| 7. Acceptance | No criterion can be measured yet | Requires the content above and device testing. |
| 8. Build and governance | Source register, validator, and release gate in place | Reproducible build manifest, diff reports, and signing are not started. |

## Content footprint audit

- Base application: not built.
- Asset packs: none. There is no approved content yet.
- Total installed playable content: **0 GB**. The 150–160 GB target has not been approached, and no padding files have been added.

## Assumptions and decisions (recorded per the specification)

1. **Project origin.** The SVY21 grid origin (E 0 m, N 0 m) is cell (0, 0) with local offset (0, 0). Coordinates are stored as metres.
2. **Axis convention.** X = easting, Y = height, Z = northing. Render space uses the same axes.
3. **Active render window.** ±8 cells (±4 km) around the origin cell. Entities outside it are not renderable and must be culled or unloaded. The window was chosen so that float32 error stays below 0.25 mm across it (measured).
4. **Vertical reference.** Not yet chosen. It must be fixed with the DEM source. Until then, heights are stored as declared but unlabelled, and no height claims are made.
5. **Maths types.** The runtime uses small dependency-free `Vec3d` and `Vec3f` structs, not glm. Engine code may map them to glm with identical layout.
6. **Language.** The runtime core is C++17, built with a plain Makefile so it can be cross-compiled with the Android NDK later. The offline pipeline is planned in Rust, and Python is used for SVY21 and validation. Changing the pipeline language later does not change the runtime or format.
7. **Test fixtures.** The coordinates in tests are approximate WGS84 locations of named places, converted with pyproj. They are fixtures for numerical and placement tests, not survey values, and they are not source data.
8. **Interface deviations from the specification.** `GetRenderCoordinate` returns `std::optional` instead of assuming validity. `ShiftPhysicsWorld` takes an `OriginShiftEvent` and a `PhysicsShiftTarget` interface, so it is not tied to a specific physics engine. `Normalize` returns a status code and leaves the value unchanged on failure.

## Next milestones, in dependency order

1. **Data access.** Verify licence and field coverage for each register candidate from a machine that can reach those services, and record retrieval dates. Pick the DEM product and the vertical reference. Nothing in §1 onward can be called "Singapore" until this is done.
2. **Pipeline core.** Install the Rust toolchain in an environment with access to it. Implement raster and vector ingest with validation and the discrepancy report (§1), writing `.sg_chunk` files with the format defined here.
3. **Chunk compression and mapping.** Add a codec with page tables, plus an Android `mmap` wrapper, behind the existing reader interface.
4. **Engine integration.** Connect the floating origin to the chosen physics engine and renderer, with the precision tests run on the device.
5. **Device validation.** Only when target hardware is available: frame-time percentiles, thermal runs, and the 60-minute traversal from §7.
