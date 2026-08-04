# Second-Pass mDNS Review, Release, and Hardware Validation

## Summary

Perform an independent review of the current uncommitted mDNS implementation, fix any concrete defects found, commit the complete feature plus fixes, flash the exact verified image without erasing NVS, and validate the device while monitoring memory pressure.

The current device is restored to `esp32-cc1101-a71d4c`, mDNS is ready, and the latest live sample is approximately 20.9 KiB free heap, 18 KiB largest block, and 3.2 KiB cumulative minimum free heap.

## Review and Fixes

- Audit mDNS owner-task enforcement, service lifecycle, generation reconciliation, failure latching, hostname conflict caching, and Wi-Fi stop/start behavior.
- Audit hostname NVS encoding, corruption fallback, persistence ordering, locking, generation updates, and STA netif application.
- Audit Host/Origin parsing, `.local` handling, OTA target validation, JSON escaping, bounded buffers, and malformed-input rejection.
- Review heap/task allocation paths around `web_service`, HTTP chunked responses, mDNS registration, and browser polling for leaks, fragmentation, or stale allocations.
- Fix only confirmed bugs, preserving existing public behavior and the restored default hostname.
- Add focused regression tests for every fix, including strict IPv4/hostname parsing, hostname persistence/application, mDNS policy transitions, and Web validation.

## Verification

Run all gates after fixes:

- Host CTest suite.
- Shell and JavaScript syntax checks.
- Fresh Unity image compilation only; do not execute Unity tests on-device.
- Clean `tools/verify-production.sh`, dependency-lock inspection, image-size, and partition checks.
- `git diff --check` and complete changed-file audit.

Memory acceptance:

- Settled free internal heap: at least 20 KiB.
- Largest internal free block: at least 16 KiB.
- `web_service` stack margin: at least 768 bytes.
- No more than 1 KiB settled free-heap degradation across repeated reconnect cycles.
- Record cumulative minimum free heap during boot, mDNS startup, repeated API polling, UI reloads, hostname changes, conflict testing, and reconnects.
- Treat allocation failures, crashes, monotonic degradation, or a low-water mark below 2 KiB as blocking findings requiring investigation before release.

## Hardware Release

- Use the confirmed classic ESP32 and stable CP2102 by-id port.
- Flash the exact image produced by the final clean verifier, without erasing NVS.
- Monitor boot ordering and confirm STA hostname application precedes Wi-Fi start, HTTP readiness precedes mDNS registration, and all mDNS errors remain `ESP_OK`.
- Validate both `http://192.0.2.17/` and `http://esp32-cc1101-a71d4c.local/`.
- Verify hostname resolution, `_http._tcp`, `_rfbridge._tcp`, exact port/TXT records, and effective conflict-name behavior.
- Repeat controlled hostname rename/reset and ten Wi-Fi stop/start cycles while collecting heap, largest-block, and stack telemetry.
- Do not transmit RF, upload OTA, erase flash, or run Unity tests.

## Commit

- Preserve the existing plan and all current mDNS implementation files; include only related review fixes.
- Commit the complete verified worktree with the message `Harden mDNS discovery and hostname management`.
- Confirm the final commit, flashed image, restored NVS hostname state, and clean post-flash device status.
- No push, rebase, amend, or destructive Git operations.
