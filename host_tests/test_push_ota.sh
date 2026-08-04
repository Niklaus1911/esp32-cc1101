#!/usr/bin/env bash

set -euo pipefail

readonly PROJECT_ROOT="$1"
readonly PUSH_OTA="$PROJECT_ROOT/tools/push-ota.sh"
test_dir="$(mktemp -d)"
trap 'rm -rf -- "$test_dir"' EXIT
readonly MOCK_BIN="$test_dir/bin"
readonly IMAGE="$test_dir/firmware.bin"
readonly CURL_LOG="$test_dir/curl.log"
readonly OUTPUT="$test_dir/output.log"
mkdir -p -- "$MOCK_BIN"
printf 'test image' >"$IMAGE"

cat >"$MOCK_BIN/esptool" <<'MOCK_ESPTOOL'
#!/usr/bin/env bash
printf '%s\n' \
    'Flash size: 4MB' \
    'Chip ID: 0 (ESP32)' \
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
        "$PUSH_OTA" "${2:-192.168.1.17}" "$IMAGE"
}

run_push success >"$OUTPUT" 2>&1
grep -q 'OTA accepted: {"ok":true,"rebooting":true}' "$OUTPUT"
grep -qx -- 'UPLOAD:--no-progress-meter' "$CURL_LOG"
grep -qx -- "UPLOAD:@$IMAGE" "$CURL_LOG"
grep -qx -- 'UPLOAD:Origin: http://192.168.1.17' "$CURL_LOG"
grep -qx -- 'UPLOAD:http://192.168.1.17/api/v1/ota' "$CURL_LOG"
if grep -q -- 'Authorization:' "$CURL_LOG"; then
    printf 'Unexpected authorization header in unauthenticated OTA request\n' >&2
    exit 1
fi

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
        "'$PUSH_OTA' 192.168.1.17 '$IMAGE'" /dev/null >"$OUTPUT" 2>&1
    if grep -Eqx -- 'UPLOAD:--progress-bar|UPLOAD:--no-progress-meter|UPLOAD:--silent' "$CURL_LOG"; then
        printf 'Interactive upload unexpectedly changed curl progress mode\n' >&2
        exit 1
    fi
fi

printf 'push-ota script tests passed\n'
