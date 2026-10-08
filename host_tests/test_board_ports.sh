#!/usr/bin/env bash

set -euo pipefail

readonly PROJECT_ROOT="$(cd -- "$1" && pwd -P)"
readonly TEST_DIR="$(mktemp -d)"
trap 'rm -rf -- "$TEST_DIR"' EXIT
readonly PORT_CONFIG="$TEST_DIR/board-ports.conf"
readonly MISSING_PORT="/dev/serial/by-id/usb-EXAMPLE_PRIVACY_TEST_MISSING-if00"
[[ ! -e "$MISSING_PORT" && ! -L "$MISSING_PORT" ]]

expect_rejected() {
    local expected="$1"
    shift
    if RFBRIDGE_PORT_CONFIG="$PORT_CONFIG" bash "$PROJECT_ROOT/tools/build-board.sh" \
            "$@" >"$TEST_DIR/output" 2>&1; then
        printf 'Unexpected hardware access acceptance: %s\n' "$*" >&2
        exit 1
    fi
    if ! grep -Fq -- "$expected" "$TEST_DIR/output"; then
        cat "$TEST_DIR/output" >&2
        printf 'Expected rejection missing: %s\n' "$expected" >&2
        exit 1
    fi
}

for action in flash monitor; do
    expect_rejected 'Hardware access for profile esp32-devkit is disabled' esp32-devkit "$action"
done

cp "$PROJECT_ROOT/tools/board-ports.example.conf" "$PORT_CONFIG"
for profile in esp32-devkit esp32s3-devkitc-n16r8 esp32s3-supermini-fh4r2 xiao-esp32s3; do
    expect_rejected "Hardware access for profile $profile is disabled" "$profile" monitor
done

for mapping in \
    'esp32-devkit CLASSIC_APPROVED_PORT' \
    'esp32s3-devkitc-n16r8 N16R8_APPROVED_PORT' \
    'esp32s3-supermini-fh4r2 SUPERMINI_FH4R2_APPROVED_PORT'; do
    read -r profile key <<< "$mapping"
    # A final line without a newline must still be parsed.
    printf '%s=%s' "$key" "$MISSING_PORT" > "$PORT_CONFIG"
    expect_rejected 'Approved serial symlink is absent' "$profile" monitor --port "$MISSING_PORT"
    expect_rejected 'Port assertion does not match the approved path' "$profile" flash \
        --port /dev/serial/by-id/usb-EXAMPLE_OTHER-if00
done

printf 'CLASSIC_APPROVED_PORT=/dev/ttyUSB0\n' > "$PORT_CONFIG"
expect_rejected 'must be an exact /dev/serial/by-id/ path' esp32-devkit monitor

printf 'CLASSIC_APPROVED_PORT=/dev/serial/by-id/../ttyUSB0\n' > "$PORT_CONFIG"
expect_rejected 'must be an exact /dev/serial/by-id/ path' esp32-devkit monitor

printf 'CLASSIC_APPROVED_PORT=%s\nCLASSIC_APPROVED_PORT=%s\n' \
    "$MISSING_PORT" "$MISSING_PORT" > "$PORT_CONFIG"
expect_rejected 'duplicate board port configuration entry' esp32-devkit monitor

printf 'UNAPPROVED_PORT=%s\n' "$MISSING_PORT" > "$PORT_CONFIG"
expect_rejected 'Invalid board port configuration key' esp32-devkit monitor

printf 'CLASSIC_APPROVED_PORT=/dev/serial/by-id/usb-$(false)-if00\n' > "$PORT_CONFIG"
expect_rejected 'Approved serial symlink is absent' esp32-devkit monitor

printf 'touch %s\n' "$TEST_DIR/executed" > "$PORT_CONFIG"
expect_rejected 'Invalid board port configuration key' esp32-devkit monitor
[[ ! -e "$TEST_DIR/executed" ]]

printf '# Private configuration\nN16R8_APPROVED_PORT=%s\n' "$MISSING_PORT" > "$PORT_CONFIG"
expect_rejected 'Hardware access for profile esp32-devkit is disabled' esp32-devkit monitor
expect_rejected 'Hardware access for profile xiao-esp32s3 is disabled' xiao-esp32s3 monitor

printf 'Board port safety checks passed\n'
