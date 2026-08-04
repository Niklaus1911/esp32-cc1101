# Replace Empty Console Continuation Tags

## Context

Pretty console events are wrapped to a 66-character body in `components/rf_console/rf_console.cpp`. Every chunk after the first currently receives a five-space tag, which renders as an empty `[     ]` label.

Replace the fake tag with an unbracketed ASCII gutter, aligned to the first line's message column:

```text
[RF RX] RC code=11043138 hex=0xA88142 bits=24 protocol=1 pulse_us=390
      | confidence=repeated repeats=2 fingerprint=0x56E2A8E7

[RULE ] TRIGGER id=3 trigger=moto_off_2 target=motore_off repeats=3
      | rx_encoding=decoded tx_encoding=decoded

[RULE ] ACTION id=3 trigger=moto_off_2 target=motore_off result=OK
      | elapsed_ms=170
```

The six-space-plus-`| ` prefix has the same eight-column display width as `[TAG  ] `, so wrapped text remains aligned without pretending that the continuation has its own label.

## Online Inspiration

- [Rust compiler diagnostics](https://doc.rust-lang.org/error_codes/E0308.html) use an aligned `|` gutter to connect follow-up annotations to the line they explain.
- [tracing-tree](https://docs.rs/tracing-tree/latest/tracing_tree/struct.HierarchicalLayer.html) exposes indentation lines and wraparound controls for readable hierarchical terminal output.
- [Rich trees](https://rich.readthedocs.io/en/stable/tree.html) use guide lines to make related terminal rows visually continuous.
- [journalctl output modes](https://www.freedesktop.org/software/systemd/man/latest/journalctl.html#Output%20Options) separate terse or structured machine output from human-readable rendering. This supports keeping the bridge's `plain` format stable while improving only `pretty` mode.

The bridge uses the ASCII `|` equivalent instead of Unicode box-drawing characters because its output travels over UART and the project documents bounded ASCII console rendering. A plain gutter is appropriate for wrapped fields; branch glyphs such as `+--` or `` `-- `` would incorrectly imply a data hierarchy.

## Implementation

1. Update `format_console_tagged_line()` in `components/rf_console/rf_console_format.cpp` so an empty or whitespace-only tag renders as a continuation: six spaces, a tone-colored `|`, one space, and the message. Preserve regular `[TAG  ]` rendering, five-column tag validation, ANSI resets, and exact plain-style output.
2. Update `print_tagged_line()` in `components/rf_console/rf_console.cpp` to pass an explicit empty tag for every chunk after the first. Keep wrapping width, chunk boundaries, locking, color tone, blank-line separation, and event payloads unchanged. This applies consistently to decoded RF frames, raw-duration lists, automation, network, and OTA events.
3. Extend `host_tests/host_tests.cpp` and `components/rf_console/test/test_rf_console_parse.cpp` with matching regression assertions for the exact gutter prefix, visible-column alignment with a normal tag, ANSI reset termination, named-tag preservation, and unchanged plain output.
4. Leave event payload formatting and `README.md` unchanged: this is a presentation-only change to `pretty` mode, so scripts and captured logs using `plain` mode retain their current stable lines.

## Verification

1. Run the portable C++17 host build and `ctest` suite.
2. Run `tools/verify-production.sh` for a clean ESP-IDF firmware build, size report, and image inspection.
3. Build the dedicated Unity image under `test_apps/unit` with ESP-IDF 6.0.2; do not flash or run on-device tests without explicit hardware approval.
4. Review the diff to confirm no generated configuration, RF behavior, event payload, or unrelated console styling changed.
