# MQTT Review Fixes

## Scope

Resolve all six findings from the MQTT implementation review as isolated, verified commits.

## Fix 1: Retained-state pending handoff

Status: completed in `d327181`.

- Consume `state_pending` when a publish cycle begins instead of clearing it after a generation check.
- Preserve any producer notification that arrives while QoS 1 publication is in progress.
- Verify with host tests and an ESP-IDF firmware build, then commit.

## Fix 2: Snapshot failure recovery

Status: completed in `8dcbc99`.

- Restore a failed last-RX or last-automation snapshot only when no newer snapshot is pending.
- Keep newer producer data authoritative during publish failures.
- Verify with host tests and an ESP-IDF firmware build, then commit.

## Fix 3: Discovery retry

Status: completed in `d662e21`.

- Track reconciliation failure in the worker and force retries until the complete discovery catalog is acknowledged.
- Preserve normal telemetry servicing while retries are pending.
- Verify with host tests and an ESP-IDF firmware build, then commit.

## Fix 4: Reconnect telemetry semantics

Status: completed in `ca30618`.

- Tag queued ephemeral telemetry with a connection epoch.
- Reject or discard messages that were produced outside the current subscribed MQTT connection.
- Continue retaining the latest state snapshot independently of ephemeral delivery.
- Verify with host tests and an ESP-IDF firmware build, then commit.

## Fix 5: Mosquitto ACL

Status: completed.

- Permit the bridge to publish every discovery component it creates while keeping access scoped to its device identifier.
- Verify the documented topic wildcard against all discovery topic formats, then commit.

## Fix 6: Reconciliation diagnostics

- Mark equal-ledger fast-path state-publication failures as faulted with the actual error.
- Ensure the retry worker and `mqtt status` observe the same failure.
- Verify with host tests and an ESP-IDF firmware build, then commit.

## Final Verification

- Run all host tests.
- Build the dedicated Unity image without running it on hardware.
- Run `tools/verify-production.sh` for a clean build, size, image, and partition check.
- Inspect the final diff, commit series, and worktree without touching unrelated files.
