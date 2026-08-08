# True-Random ASK/OOK Signal Generation

## Summary

Add a shared operation that generates a hardware-random, rc-switch-compatible decoded signal, saves it to NVS, and exposes it through the learned-signal catalog. Provide a one-click Web action and a `random` UART command. Generation never transmits RF.

## Interfaces And Behavior

- Add `network_wifi_get_true_random_word(uint32_t *)`. Execute the RNG read on the Wi-Fi owner task and succeed only while `esp_wifi_start()` is active, as required by ESP-IDF 6.0.2 for guaranteed true randomness. Do not use pseudo-random or temporary SAR fallback.
- Add `bridge_control_generate_and_save_random_decoded(source, result)` so Web and UART share entropy acquisition, collision handling, persistence, and event publication.
- Generate a uniform 24-bit code, including zero, using protocol 1 and its nominal 350 us pulse width.
- Name it `random_%06X`, for example `random_D30A91`.
- Reject semantic collisions with existing learned signals and exact name collisions. Draw up to eight candidates, never overwrite, then return a bounded exhaustion error.
- Save through `rf_signals_save_decoded()` so the catalog refreshes and MQTT/Home Assistant creates the normal replay button.
- Do not change the latest frame, recent history, learning state, active RF backend, or transmit state.

## Web And UART

- Add `POST /api/signals/random` with an empty body, exact same-origin validation, and `201 Created` on success.
- Return the saved record:
  `{"ok":true,"signal":{"name":"random_D30A91","encoding":"decoded","code":"0xD30A91","code_decimal":"13830801","bits":24,"protocol":1,"pulse_us":350}}`
- Return `409 true_random_unavailable` if Wi-Fi is not started and `503 random_generation_exhausted` after eight collisions. Preserve existing mappings for NVS and service failures.
- Increase the HTTP handler capacity from 21 to 22.
- Add **Generate & save** beside Refresh under **Signals > Learned signals**. Show the generated name/code, then refresh the learned list and automation selectors.
- Add the no-argument UART command `random`. Plain output is:
  `RANDOM name=random_D30A91 code=13830801 hex=0xD30A91 bits=24 protocol=1 pulse_us=350`
- When Wi-Fi is off, print an actionable true-entropy-unavailable error instructing the user to start/connect Wi-Fi.
- Document the command, Web action, fixed format, automatic naming, entropy requirement, Home Assistant behavior, and no-transmit guarantee in the existing README without discarding current uncommitted documentation work.

## Test Plan

- Add host tests for bit masking, zero/high boundary values, canonical protocol timing, padded names, valid storage names, collision retries, exhaustion, and dependency error propagation.
- Extend Web asset/API contracts for the button, route, response fields, same-origin enforcement, empty-body rejection, handler count, refresh behavior, and absence of transmit calls.
- Compile Unity images for ESP32 and ESP32-S3.
- Run focused host tests, then the final unchanged-candidate `tools/verify-production.sh`.
- Validate desktop and mobile Web layouts with a deterministic Playwright mock, including success, entropy/storage errors, refresh behavior, console/page errors, and overflow screenshots.
- Using the authorized classic ESP32 profile and approved by-id port, flash and monitor through `tools/build-board.sh`; exercise the UART entropy rejection and successful save/list paths when existing Wi-Fi configuration permits. Do not replay or transmit the generated signal during validation.

## Assumptions

- The selected format is a random 24-bit code with fixed protocol 1, not random raw timings or random protocols.
- UART generation requires the Wi-Fi RF entropy source; offline DRBG and SAR entropy modes are out of scope.
- Both CC1101 and generic ASK/OOK backends can later replay the saved decoded signal.
- No MQTT generation command is added; MQTT/Home Assistant receives only the resulting learned-signal replay button.
- No Git commit is part of this feature unless separately requested.
