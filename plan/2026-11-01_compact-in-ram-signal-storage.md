# Compact in-RAM signal storage

## Goal

Free internal RAM on the classic ESP32 (no PSRAM) without lowering any limit. The learned-signal catalog (32 × 560 B) and the automation rule table (32 rules × 1,136 B) embed a full `RfStoredSignal` for every signal. Each one carries a 516-byte `RawSignal` that decoded signals never use.

## Design

- Add `RfCompactSignal` to `rf_storage_format.hpp`, holding the encoding, an inline `DecodedSignal`, and `RawSignal *raw`. `raw` is owned and allocated only for raw signals.
  - `rf_compact_signal_assign(out, stored, spare)` copies a stored signal. A raw signal takes `*spare` if given, otherwise it allocates (PSRAM first on S3).
  - `rf_compact_signal_release()` frees the raw buffer and resets the signal to the zeroed decoded state.
  - `rf_compact_raw_allocate()` / `rf_compact_raw_free()` handle buffers reserved ahead of time.
  - The all-zero bytes represent a valid empty signal, so the zero-filled PSRAM rule storage stays valid.
- Overload `rf_stored_signals_equivalent` for stored/compact, compact/stored and compact/compact, sharing one implementation. Overload `rf_find_unique_stored_signal_match` for compact triggers. Call sites keep their names.
- Catalog: `LearnedSignalEntry::signal` becomes `RfCompactSignal`. Refresh releases every slot, then loads each signal through one heap-allocated temporary `RfStoredSignal` (no extra stack) and assigns it.
- Rules: `RuntimeRule` trigger and target become `RfCompactSignal`.
  - `clear_runtime_rules()` releases every slot, including a partially loaded one.
  - Remove releases the removed rule before the memmove.
  - Load uses a heap temporary.
  - Add reserves both raw buffers before persisting, so an allocation failure can never leave a rule in NVS without a runtime copy.
  - Firing transmits `*target.raw` for raw targets.
- The NVS formats are unchanged; only the in-RAM representation changes.

## Expected effect

- The catalog shrinks from 17.9 KB to about 1.8 KB and the rules from 36.4 KB to about 4.4 KB.
- Each raw signal costs about 520 B of heap while loaded.
- The worst case (everything raw) matches today's total.
- On the classic ESP32 this frees about 48 KB of static RAM; on S3 it shrinks the PSRAM tables.

## Verification

- Host tests:
  - compact assign/release round trips for decoded and raw signals;
  - equivalence across all type combinations;
  - catalog matching with compact entries, including raw.
- Unity tests updated for the new entry type.
- Unity builds for both targets and the production verifier.
- On the classic board (wired, approved port):
  - free and largest internal RAM before and after;
  - learned decoded and raw signals load and match;
  - rule add/remove with no RF effect: rules are only created, never triggered, and no module is connected.
- On the N16R8 (OTA): signals and matching with a real remote.
