# Named NVS learning and replay

## Behavior

Add these UART workflows while preserving the existing RAM-only commands:

```text
learn <name>
learn list
forget <name>
replay <name> <repeats>

# Existing behavior remains valid:
last
replay
replay <repeats>
```

`learn <name>` arms a one-shot capture of the **next accepted decoded or raw frame** for 30 seconds. After capture, the frame is validated and committed to NVS. If no accepted frame arrives before the deadline, learning is cancelled, no NVS record is changed, and `LEARN TIMEOUT name=...` is printed. If the name already exists, the command fails without arming and never overwrites the stored record. `learn list` prints saved names, and `forget <name>` deletes one committed record. `replay <name> <repeats>` loads and validates the persistent frame, then transmits it 1–20 times without replacing the latest RAM RX frame.

Names will be 1–15 characters, start with an ASCII letter, and contain only letters, digits, `_`, or `-`, matching the NVS key limit and avoiding replay-argument ambiguity. `list` is reserved by the `learn list` subcommand.

## Implementation

### 1. Add a versioned storage component

Create `components/rf_storage/` with public APIs to initialize storage, validate names, test existence, save a new `RfFrame`, load one by name, enumerate names, and erase one name.

- Use the default NVS partition and a dedicated namespace such as `rf_codes`.
- Initialize NVS from `main/main.cpp`. Never erase NVS automatically: `NO_FREE_PAGES`, `NEW_VERSION_FOUND`, and all other initialization failures disable persistent commands with a clear error while RF and the console remain operational.
- Serialize an explicit magic/version/encoding/length record rather than persisting C++ struct layout.
- Store only replay-relevant fields: decoded code/bits/protocol/pulse/inversion or raw start level/count/durations.
- On load, reject wrong magic/version/length, truncated blobs, invalid enum values, malformed decoded/raw signals, and trailing data. Recompute runtime metadata/fingerprint instead of trusting persisted derived fields.
- Serialize NVS existence/save/load/list/erase calls with a component-owned mutex; commit writes and erases and close handles on every path.
- Enforce create-only learning in the storage backend as well as at command arming, so a race or delayed frame can never overwrite an existing key. Return an explicit already-exists error without changing the old record.

Files:

- `components/rf_storage/CMakeLists.txt`
- `components/rf_storage/include/rf_storage.hpp`
- `components/rf_storage/rf_storage.cpp`
- `components/rf_storage/rf_storage_format.cpp` (portable codec/name validation)
- `components/rf_storage/test/test_rf_storage.cpp`
- `main/CMakeLists.txt`, `main/main.cpp`

### 2. Add ordered asynchronous learning to the console

Update `components/rf_console/rf_console.cpp` and its component dependency:

- Register `learn <name>`, `learn list`, and `forget <name>`, and extend `replay` to accept the three-argument named form.
- Require RF to be running with desired RX enabled before arming learning.
- Replace the printer queue payload with tagged events for frames and learn requests. The printer/worker task exclusively owns the pending learn name, so captures already queued before `learn` cannot be saved accidentally.
- Keep `rf_console_on_frame()` nonblocking and free of NVS work; it only queues the accepted frame so the radio-owner task is never stalled by flash writes.
- If the event queue is full, fail a learn request with a bounded wait; dropped RX print events retain the existing counter, and a pending learn remains armed until a later accepted frame arrives.
- Store a monotonic 30-second deadline with each learn request. Timestamp frame events when queued so a frame received before the deadline can still be accepted if worker output/NVS activity delays processing.
- Before arming, reject an existing name without disturbing any current pending learn request. While learning is pending, bound the worker's queue wait by the remaining deadline. On expiry, clear the request and print `LEARN TIMEOUT name=...` without modifying NVS. A valid newer `learn` request replaces the pending name and starts a fresh 30-second window.
- On the first frame queued within the window, consume the one-shot request, save it through `rf_storage`, and print an explicit `LEARNED name=...` or storage error result. Frames queued after expiry remain ordinary RX events and must not be saved.
- Enumerate stored keys for `learn list`; print an explicit empty result when none exist. `forget` validates the name, commits the erase, and reports missing keys without affecting other records.
- Load named frames in REPL task context and route decoded/raw records through the existing bounded transmit APIs. Missing/corrupt names and invalid repeat counts must fail without transmitting.

Update parser helpers as needed in:

- `components/rf_console/include/rf_console_parse.hpp`
- `components/rf_console/rf_console_parse.cpp`
- `components/rf_console/test/test_rf_console_parse.cpp`

### 3. Match the firmware image header to the 4 MB board

Update both `sdkconfig.defaults` and `test_apps/unit/sdkconfig.defaults` from `CONFIG_ESPTOOLPY_FLASHSIZE_2MB=y` to `CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y`.

- Regenerate/reconfigure the ignored local `sdkconfig` for 4 MB before the next hardware build; do not commit generated configuration.
- Keep the existing partition layout unchanged—the setting corrects the image header and does not move or erase NVS/application partitions.
- Document that the project defaults target the detected 4 MB DevKit flash and that an actual 2 MB board must select its real size in `menuconfig`.
- Verify the built image reports 4 MB and boot no longer prints `Detected size(4096k) larger than ... header(2048k)`.

### 4. Tests and documentation

- Add portable round-trip tests for decoded and maximum-size raw records.
- Add malformed-record tests for bad magic/version/encoding, invalid lengths/counts, truncated/trailing data, and invalid signal payloads.
- Test name boundaries/characters (including reserved `list`), create-only/no-overwrite behavior, list/forget results, the 30-second learn deadline (before/at/after expiry and replacement), and named replay repeat parsing, including preservation of the existing `replay [repeats]` forms.
- Include the storage tests in `host_tests/CMakeLists.txt`, `host_tests/host_tests.cpp`, and the ESP-IDF Unity image under `test_apps/unit/main/`.
- Update `README.md` to remove the no-NVS claim and document next-signal learning, no-overwrite behavior, list/forget commands, name rules, persistence, timeout, errors/capacity, and examples.

## Verification

1. Run normal and ASan/UBSan host tests.
2. Build the ESP-IDF Unity image and production firmware cleanly under ESP-IDF 6.0.2 from regenerated 4 MB configurations; inspect image metadata, run `idf.py size`, and inspect diagnostics/diff hygiene.
3. Do not flash automatically. With explicit approval, validate on hardware:
   - boot without the 4 MB-versus-2 MB image-header warning;
   - learn decoded and raw remotes;
   - reboot and replay each by name with requested repeat counts;
   - attempt to learn an existing name and confirm it fails without arming or overwriting;
   - list saved names, forget one, and confirm only that record is removed;
   - let learning expire for 30 seconds and confirm the timeout message appears and no later frame is saved;
   - reject missing, malformed, reserved, and overlength names without transmitting;
   - verify ordinary RX, `last`, `replay [repeats]`, and direct send/raw commands still work.
