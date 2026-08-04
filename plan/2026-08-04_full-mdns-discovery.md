# Full mDNS Discovery and Hostname Management

## Summary

Add persistent, collision-aware `.local` discovery for the classic ESP32, advertising the Web UI through `_http._tcp` and a private `_rfbridge._tcp` service. The DHCP and mDNS hostname share one canonical configuration, defaulting to `esp32-cc1101-<last-3-STA-MAC-bytes>`.

Use Espressif `mdns` 1.11.3 exactly. All potentially unbounded vendor calls run exclusively on the existing `web_service` task after HTTP is ready, keeping Wi-Fi, RF, UART, and HTTP request handling independent.

## Identity And Public Interfaces

- Add a version-1 `net_cfg/hostname` NVS blob owned by `network_wifi`: magic `NHCF`, version, length, two zero reserved bytes, lowercase hostname payload, and little-endian CRC32. Missing data selects the default; malformed data is retained, reported as a persistence error, and falls back to the default without erasing NVS.
- Accept 1–32 ASCII letters or digits with hyphens only in interior positions. Canonicalize to lowercase; reject dots, `.local`, underscores, whitespace, non-ASCII, and leading/trailing hyphens. Setting the MAC-derived default is equivalent to reset and removes the override.
- Add `NetworkHostnameStatus` with default/configured hostname, custom flag, configured and netif-applied generations, persistence error, and last netif-apply error. Derive the default with `esp_read_mac(..., ESP_MAC_WIFI_STA)` and fail Wi-Fi initialization if the MAC cannot be read.
- Apply the configured hostname when creating the STA netif, immediately after successful runtime changes, and before every `esp_wifi_start`. Runtime changes do not reconnect Wi-Fi; DHCP advertises the new value on the next connection.
- Add `network_mdns` APIs: `initialize_network_mdns()`, owner-only `start_network_mdns(uint16_t)`, owner-only `reconcile_network_mdns()`, `set_network_mdns_hostname()`, `reset_network_mdns_hostname()`, and `get_network_mdns_status()`.
- Add `NetworkMdnsStatus` with `not_started`, `starting`, `ready`, `faulted`, and computed `stalled` states; configured/effective names; custom, effective-known, and conflict-renamed flags; per-service registration flags; configured/applied generations; owner heartbeat age; initialization and last errors. Registration must never be described as proof that packets were advertised.

## mDNS Runtime And Services

- Initialize the application component after Wi-Fi and before Web UI startup, but defer `mdns_init()` until the first successful HTTP start has registered every Web and OTA handler and marked the server ready.
- Record `web_service` as the sole vendor-API owner. Hostname setters persist/update shared desired state and notify the Wi-Fi and Web owner tasks without invoking mDNS directly. Reconcile after notifications and every second once mDNS is active, including while offline.
- Run `mdns_init`, hostname set/get, and service registration only on that owner. Poll `mdns_hostname_get()` to cache conflict-renamed effective names. If the heartbeat is older than five seconds while starting or ready, report `stalled`; never start another owner or issue concurrent vendor calls, and require reboot for recovery.
- Keep successful initialization and services for the process lifetime. Do not routinely call `mdns_free`, remove/re-add services, or recreate allocations across Wi-Fi loss; predefined STA event hooks handle disable and reannouncement.
- Latch returned failures for the current HTTP lifecycle and retry incomplete initialization or registration only after a later successful HTTP restart. Runtime rename failures remain visible and retry on the next Web lifecycle or a new hostname generation.
- Register port 80 with stable instance `ESP32 CC1101 RF Bridge <MAC-SUFFIX>`:
  - `_http._tcp`: `path=/`
  - `_rfbridge._tcp`: `txtvers=1`, `path=/`, `api=/api/live`, `ota=/api/v1/ota`, `project=esp32-cc1101`, running application `version`, `features=rx,tx,learn,replay,rules,ota`, and `auth=none`
- Keep the instance independent of hostname overrides. Let upstream resolve hostname and instance conflicts, expose only the effective hostname, and do not invent an instance getter or conflict counter.
- Pin `espressif/mdns: "1.11.3"` and configure production and Unity defaults with two allocated interface slots, two services, queue depth 8, 4096-byte upstream task stack, predefined STA enabled, AP and Ethernet disabled, console CLI disabled, and multiple-instance mode disabled. The otherwise-unused second interface slot is required because 1.11.3's duplicate-interface check unconditionally initializes and indexes two entries even when only STA is predefined.

## Console, Web, And Tooling

- Add UART commands `hostname status`, `hostname set <label>`, and `hostname reset`. Status reports shared DHCP/mDNS identity, generations, effective conflict name, service registration, and errors. Update README discovery, command, security, and OTA instructions.
- Add an `mdns` object to `/api/live` with the status fields above and add `errors.mdns` for snapshot retrieval. Reuse the existing System Services and diagnostics sections to show readiness and the effective `http://<hostname>.local/` URL; add no Web mutation or new card.
- Preserve the strict `app.js < 32 KiB` contract by consolidating existing System rendering while adding the mDNS rows.
- Extend Host validation to accept the current numeric IPv4, configured `<hostname>.local`, or cached effective `<hostname>.local`, with optional `:80`. Use case-insensitive DNS-label comparison and buffers sized for the upstream 64-character effective name plus `.local:80`.
- Parse Host into one validated normalized representation, then require every mutation Origin to match that exact representation. Reject cross-matches between two otherwise allowed names, HTTPS, alternate ports, paths, userinfo, opaque origins, arbitrary domains, extra dots, and malformed labels.
- Extend `tools/push-ota.sh` to accept canonical IPv4 or one 1–32-character `<hostname>.local` label, normalizing case while rejecting ports, paths, trailing dots, whitespace, metacharacters, and malformed input. Preserve exact same-origin headers and all existing image validation.
- Add the component to production and Unity builds, retain generated root and Unity `dependencies.lock` files, never edit `managed_components`, and extend production verification to assert the exact dependency and mDNS Kconfig bounds.

## Test And Acceptance Plan

- Add host tests for hostname grammar/canonicalization, MAC defaults, exact record layout, round trips, CRC/version/size/reserved-byte failures, default/reset behavior, generation reconciliation, and latched fault/stall state transitions using a thin injectable vendor adapter or pure policy layer.
- Add Unity coverage for NVS missing/corrupt/wrong-type handling, own-key repair, persistence-first updates, and STA-netif application. Compile the Unity image but do not execute it on-device.
- Expand Web tests for `/api/live`, System rendering, effective conflict names, IPv4 and `.local` Host/Origin combinations, exact same-origin enforcement, malicious inputs, reconnect compatibility, and the 32 KiB JavaScript limit.
- Expand OTA shell tests for valid IPv4 and `.local` targets, canonical URLs/Origin headers, boundaries, and injection/path/port/trailing-dot rejection.
- Run host CTest, shell and JavaScript syntax checks, the ESP-IDF compiler build, Unity compilation, `tools/verify-production.sh`, and dependency-lock inspection.
- Use Playwright first against a deterministic local mock, then the device via IPv4 and `.local`; inspect desktop/mobile screenshots, overflow, console/page errors, polling, stale/reconnect behavior, and intercept all RF, destructive, and OTA mutations.
- Before hardware access, rediscover the CP2102 with `tio --list`; use the stable by-id port for a normal no-erase flash and monitor the classic ESP32 DevKit at 115200 baud. Do not transmit RF, upload OTA, erase flash, regenerate configuration, or run Unity tests.
- Confirm HTTP becomes ready before mDNS allocation, both names resolve with `avahi-resolve-host-name` and `getent`, and `avahi-browse` returns both services with exact port, instance, and TXT records.
- Capture the original hostname/custom state. Verify a temporary rename updates mDNS and Web access without a Wi-Fi disconnect, then reconnect and confirm the hostname was applied before DHCP startup and, where observable, in the DHCP lease or packet exchange.
- Publish a temporary conflicting name from the host, apply it to the device, verify the cached effective `-2` name and Web access, stop the publisher, and restore the exact original hostname state even if validation fails.
- Run ten stop/start reconnect cycles. Require automatic service reannouncement, at least 20 KiB free internal heap, a 16 KiB largest block, at least 768 bytes `web_service` stack margin, and no greater than 1 KiB settled free-heap degradation between the first and final cycle.
- Save this plan body as `plan/2026-08-04_full-mdns-discovery.md` before implementation begins in the first write-enabled turn.
