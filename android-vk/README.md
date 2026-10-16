# Singapore Vulkan pilot (Android)

Native Vulkan app. Built by `.github/workflows/build-vulkan-apk.yml`; the APK is attached to a `vk-run-N` pre-release.

- **Orientation:** landscape only (`android:screenOrientation="landscape"` in `app/src/main/AndroidManifest.xml`).
- **Controls:** one finger drags to move; pinch zooms about the fingers; twist turns; vertical two-finger drag tilts; a short tap reads the ground position; a double tap zooms in on that point.
- **Readout:** centre position, last tap position, screen size and turn, frame rate, and tap count.
- **Host tests:** `tests/host_test.cpp` builds on a desktop compiler (see the workflow for the exact command).
