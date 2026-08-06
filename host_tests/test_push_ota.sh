#!/usr/bin/env bash

set -euo pipefail

readonly PROJECT_ROOT="$1"
readonly PUSH_OTA="$PROJECT_ROOT/tools/push-ota.sh"
test_dir="$(mktemp -d)"
trap 'rm -rf -- "$test_dir"' EXIT
readonly MOCK_BIN="$test_dir/bin"
readonly CLASSIC_IMAGE="$test_dir/classic.bin"
readonly XIAO_IMAGE="$test_dir/xiao.bin"
readonly N16R8_IMAGE="$test_dir/n16r8.bin"
readonly INVALID_IMAGE="$test_dir/invalid.bin"
readonly OVERSIZED_IMAGE="$test_dir/oversized.bin"
readonly TINY_IMAGE="$test_dir/tiny.bin"
readonly CURL_LOG="$test_dir/curl.log"
readonly OUTPUT="$test_dir/output.log"
mkdir -p -- "$MOCK_BIN"

make_image() {
    local image="$1"
    local profile="$2"
    dd if=/dev/zero of="$image" bs=512 count=1 status=none
    case "$profile" in
        classic)
            printf '\x52\x46\x42\x44\x01\x10\x01\x01\x04\x01\x00\x00\x00\x00\x00\x00'
            ;;
        xiao)
            printf '\x52\x46\x42\x44\x01\x10\x03\x02\x08\x02\x00\x00\x00\x00\x00\x00'
            ;;
        n16r8)
            printf '\x52\x46\x42\x44\x01\x10\x02\x02\x10\x03\x00\x00\x00\x00\x00\x00'
            ;;
        invalid)
            printf '\x00\x46\x42\x44\x01\x10\x02\x02\x10\x03\x00\x00\x00\x00\x00\x00'
            ;;
    esac | dd of="$image" bs=1 seek=288 conv=notrunc status=none
}

make_image "$CLASSIC_IMAGE" classic
make_image "$XIAO_IMAGE" xiao
make_image "$N16R8_IMAGE" n16r8
make_image "$INVALID_IMAGE" invalid
cp -- "$N16R8_IMAGE" "$OVERSIZED_IMAGE"
truncate -s $((0x7e0001)) "$OVERSIZED_IMAGE"
printf 'tiny' >"$TINY_IMAGE"

cat >"$MOCK_BIN/esptool" <<'MOCK_ESPTOOL'
#!/usr/bin/env bash
case "${MOCK_IMAGE_METADATA:-classic}" in
    classic)
        flash='Flash size: 4MB'
        chip='Chip ID: 0 (ESP32)'
        ;;
    xiao)
        flash='Flash size: 8MB'
        chip='Chip ID: 9 (ESP32-S3)'
        ;;
    n16r8)
        flash='Flash size: 16MB'
        chip='Chip ID: 9 (ESP32-S3)'
        ;;
esac
printf '%s\n' \
    "$flash" \
    "$chip" \
    'Checksum: 0xea (valid)' \
    'Validation hash: 33b46e331793f9854a4a493b5c4bcc0d846b752d5680451a72071bafb39b4a03 (valid)' \
    'Project name: esp32-cc1101' \
    'ESP-IDF: v6.0.2'
MOCK_ESPTOOL

cat >"$MOCK_BIN/curl" <<'MOCK_CURL'
#!/usr/bin/env bash
set -euo pipefail
arguments=("$@")
printf '%s\n' "$@" >>"$MOCK_CURL_LOG"
output=""
url="${!#}"
while (($#)); do
    if [[ "$1" == "--output" ]]; then
        output="$2"
        shift 2
    else
        shift
    fi
done
if [[ "$url" == */status ]]; then
    printf '{"state":"idle","server":true}' >"$output"
    exit 0
fi
for argument in "${arguments[@]}"; do
    printf 'UPLOAD:%s\n' "$argument" >>"$MOCK_CURL_LOG"
done
case "${MOCK_UPLOAD_MODE:-success}" in
    success)
        printf '{"ok":true,"rebooting":true}' >"$output"
        ;;
    fail)
        printf '{"error":"ESP_FAIL","code":-1}' >"$output"
        exit 22
        ;;
    transport)
        exit 7
        ;;
    malformed)
        printf '{"ok":false}' >"$output"
        ;;
esac
MOCK_CURL
chmod +x "$MOCK_BIN/esptool" "$MOCK_BIN/curl"

run_push() {
    PATH="$MOCK_BIN:$PATH" MOCK_CURL_LOG="$CURL_LOG" MOCK_UPLOAD_MODE="${1:-success}" \
        MOCK_IMAGE_METADATA="${4:-classic}" \
        "$PUSH_OTA" "${2:-192.168.1.17}" "${3:-$CLASSIC_IMAGE}"
}

run_push success >"$OUTPUT" 2>&1
grep -q 'OTA accepted: {"ok":true,"rebooting":true}' "$OUTPUT"
grep -q 'Image profile: esp32-devkit; OTA slot limit: 0x1e0000 bytes' "$OUTPUT"
grep -qx -- 'UPLOAD:--no-progress-meter' "$CURL_LOG"
grep -qx -- "UPLOAD:@$CLASSIC_IMAGE" "$CURL_LOG"
grep -qx -- 'UPLOAD:Origin: http://192.168.1.17' "$CURL_LOG"
grep -qx -- 'UPLOAD:http://192.168.1.17/api/v1/ota' "$CURL_LOG"
if grep -q -- 'Authorization:' "$CURL_LOG"; then
    printf 'Unexpected authorization header in unauthenticated OTA request\n' >&2
    exit 1
fi

: >"$CURL_LOG"
run_push success 192.168.1.17 "$XIAO_IMAGE" xiao >"$OUTPUT" 2>&1
grep -q 'Image profile: xiao-esp32s3; OTA slot limit: 0x3e0000 bytes' "$OUTPUT"
grep -qx -- "UPLOAD:@$XIAO_IMAGE" "$CURL_LOG"

: >"$CURL_LOG"
run_push success 192.168.1.17 "$N16R8_IMAGE" n16r8 >"$OUTPUT" 2>&1
grep -q 'Image profile: esp32s3-devkitc-n16r8; OTA slot limit: 0x7e0000 bytes' "$OUTPUT"
grep -qx -- "UPLOAD:@$N16R8_IMAGE" "$CURL_LOG"

if run_push success 192.168.1.17 "$INVALID_IMAGE" n16r8 >"$OUTPUT" 2>&1; then
    printf 'Expected an unsupported board descriptor to fail\n' >&2
    exit 1
fi
grep -q 'missing or unsupported RFBD board descriptor' "$OUTPUT"

if run_push success 192.168.1.17 "$N16R8_IMAGE" classic >"$OUTPUT" 2>&1; then
    printf 'Expected mismatched N16R8 metadata to fail\n' >&2
    exit 1
fi
grep -q 'missing esp32s3-devkitc-n16r8 flash header' "$OUTPUT"

if run_push success 192.168.1.17 "$OVERSIZED_IMAGE" n16r8 >"$OUTPUT" 2>&1; then
    printf 'Expected an oversized N16R8 image to fail\n' >&2
    exit 1
fi
grep -q 'outside the esp32s3-devkitc-n16r8 OTA slot limit' "$OUTPUT"

if run_push success 192.168.1.17 "$TINY_IMAGE" n16r8 >"$OUTPUT" 2>&1; then
    printf 'Expected an image without a descriptor to fail\n' >&2
    exit 1
fi
grep -q 'too small to contain the board descriptor' "$OUTPUT"

: >"$CURL_LOG"
run_push success ESP32-CC1101-A1B2C3.local >"$OUTPUT" 2>&1
grep -qx -- 'UPLOAD:Origin: http://esp32-cc1101-a1b2c3.local' "$CURL_LOG"
grep -qx -- 'UPLOAD:http://esp32-cc1101-a1b2c3.local/api/v1/ota' "$CURL_LOG"

: >"$CURL_LOG"
run_push success 192.168.001.017 >"$OUTPUT" 2>&1
grep -qx -- 'UPLOAD:Origin: http://192.168.1.17' "$CURL_LOG"

valid_max="$(printf 'a%.0s' {1..32}).local"
run_push success "$valid_max" >"$OUTPUT" 2>&1
for invalid in \
    "$(printf 'a%.0s' {1..33}).local" \
    -bridge.local bridge-.local bridge..local bridge.local. bridge.example.local \
    'bridge.local:80' 'bridge.local/path' 'bridge_name.local' 'bridge local' \
    'bridge.local;touch-x' 192.168.1 192.168.1.256 192.168.1.17.; do
    if run_push success "$invalid" >"$OUTPUT" 2>&1; then
        printf 'Expected invalid address to fail: %s\n' "$invalid" >&2
        exit 1
    fi
    grep -q 'Invalid ESP32 address' "$OUTPUT"
done

set +e
run_push fail >"$OUTPUT" 2>&1
upload_status=$?
set -e
[[ $upload_status -eq 22 ]]
grep -q 'OTA upload failed: {"error":"ESP_FAIL","code":-1}' "$OUTPUT"

set +e
run_push transport >"$OUTPUT" 2>&1
upload_status=$?
set -e
[[ $upload_status -eq 7 ]]
grep -q '^OTA upload failed$' "$OUTPUT"

if run_push malformed >"$OUTPUT" 2>&1; then
    printf 'Expected malformed success response to return nonzero\n' >&2
    exit 1
fi
grep -q 'OTA service did not confirm success' "$OUTPUT"

if command -v script >/dev/null 2>&1 && script --help 2>&1 | grep -q -- '--command'; then
    : >"$CURL_LOG"
    PATH="$MOCK_BIN:$PATH" MOCK_CURL_LOG="$CURL_LOG" MOCK_UPLOAD_MODE=success \
        script --quiet --return --command \
        "'$PUSH_OTA' 192.168.1.17 '$CLASSIC_IMAGE'" /dev/null >"$OUTPUT" 2>&1
    if grep -Eqx -- 'UPLOAD:--progress-bar|UPLOAD:--no-progress-meter|UPLOAD:--silent' "$CURL_LOG"; then
        printf 'Interactive upload unexpectedly changed curl progress mode\n' >&2
        exit 1
    fi
fi

printf 'push-ota script tests passed\n'
