#!/usr/bin/env bash

set -euo pipefail

readonly OTA_PORT="${OTA_HTTP_PORT:-80}"
readonly CONFIRM_TIMEOUT_SECONDS="${OTA_REBOOT_TIMEOUT_SECONDS:-120}"
readonly POLL_INTERVAL_SECONDS="${OTA_POLL_INTERVAL_SECONDS:-2}"
readonly IDF_EXPORT="$HOME/.espressif/v6.0.2/esp-idf/export.sh"
readonly CONNECT_TIMEOUT_SECONDS=5
readonly UPLOAD_TIMEOUT_SECONDS=900
readonly BOARD_DESCRIPTOR_OFFSET=$((24 + 8 + 256))

usage() {
    printf 'Usage: %s [--no-wait] <esp32-ipv4-or-hostname.local> <application-image.bin>\n' \
        "${0##*/}" >&2
}

wait_for_confirmation=true
if [[ ${1:-} == "--no-wait" ]]; then
    wait_for_confirmation=false
    shift
fi
if [[ $# -ne 2 ]]; then
    usage
    exit 2
fi

readonly DEVICE_INPUT="$1"
readonly IMAGE="$2"

device_host=""
lower_input="${DEVICE_INPUT,,}"
if [[ "$lower_input" == *.local ]]; then
    if [[ ! "$lower_input" =~ ^([a-z0-9]|[a-z0-9][a-z0-9-]{0,30}[a-z0-9])\.local$ ]]; then
        printf 'Invalid ESP32 address: %s\n' "$DEVICE_INPUT" >&2
        exit 2
    fi
    device_host="$lower_input"
else
    if [[ ! "$DEVICE_INPUT" =~ ^[0-9]{1,3}(\.[0-9]{1,3}){3}$ ]]; then
        printf 'Invalid ESP32 address: %s\n' "$DEVICE_INPUT" >&2
        exit 2
    fi
    IFS='.' read -r -a octets <<<"$DEVICE_INPUT"
    if [[ ${#octets[@]} -ne 4 ]]; then
        printf 'Invalid ESP32 address: %s\n' "$DEVICE_INPUT" >&2
        exit 2
    fi
    canonical_octets=()
    for octet in "${octets[@]}"; do
        if [[ ! "$octet" =~ ^[0-9]{1,3}$ ]] || ((10#$octet > 255)); then
            printf 'Invalid ESP32 address: %s\n' "$DEVICE_INPUT" >&2
            exit 2
        fi
        canonical_octets+=("$((10#$octet))")
    done
    device_host="${canonical_octets[0]}.${canonical_octets[1]}.${canonical_octets[2]}.${canonical_octets[3]}"
fi
readonly DEVICE_HOST="$device_host"
if [[ ! "$OTA_PORT" =~ ^[0-9]+$ ]] || ((OTA_PORT < 80 || OTA_PORT > 65535)); then
    printf 'Invalid OTA_HTTP_PORT: %s\n' "$OTA_PORT" >&2
    exit 2
fi
if [[ ! "$CONFIRM_TIMEOUT_SECONDS" =~ ^[0-9]+$ ]]; then
    printf 'Invalid OTA_REBOOT_TIMEOUT_SECONDS: %s\n' "$CONFIRM_TIMEOUT_SECONDS" >&2
    exit 2
fi
if [[ ! "$POLL_INTERVAL_SECONDS" =~ ^[0-9]+([.][0-9]+)?$ ]]; then
    printf 'Invalid OTA_POLL_INTERVAL_SECONDS: %s\n' "$POLL_INTERVAL_SECONDS" >&2
    exit 2
fi
if [[ ! -f "$IMAGE" || ! -r "$IMAGE" ]]; then
    printf 'Firmware image is not a readable regular file: %s\n' "$IMAGE" >&2
    exit 2
fi
readonly IMAGE_SIZE="$(wc -c <"$IMAGE")"
if ((IMAGE_SIZE < BOARD_DESCRIPTOR_OFFSET + 16)); then
    printf 'Firmware image is too small to contain the board descriptor: %s bytes\n' \
        "$IMAGE_SIZE" >&2
    exit 2
fi
for command in curl od python3; do
    if ! command -v "$command" >/dev/null 2>&1; then
        printf '%s is required\n' "$command" >&2
        exit 1
    fi
done

inspect_image() {
    if command -v esptool >/dev/null 2>&1; then
        esptool image-info "$IMAGE"
    elif [[ -f "$IDF_EXPORT" ]]; then
        bash -c 'set +u; source "$1" >/dev/null; set -u; "$IDF_PYTHON_ENV_PATH/bin/python" -m esptool image-info "$2"' \
            bash "$IDF_EXPORT" "$IMAGE"
    else
        printf 'esptool is unavailable and ESP-IDF 6.0.2 cannot be exported\n' >&2
        return 1
    fi
}

require_image_metadata() {
    local pattern="$1"
    local description="$2"
    if ! grep -Eq -- "$pattern" "$image_info_file"; then
        printf 'Image validation failed: missing %s\n' "$description" >&2
        return 1
    fi
}

require_exact_image_metadata() {
    local line="$1"
    local description="$2"
    if ! grep -Fqx -- "$line" "$image_info_file"; then
        printf 'Image validation failed: missing %s\n' "$description" >&2
        return 1
    fi
}

json_field() {
    python3 -c '
import json
import sys
with open(sys.argv[1], encoding="utf-8") as source:
    value = json.load(source)
for key in sys.argv[2].split("."):
    if not isinstance(value, dict) or key not in value:
        sys.exit(0)
    value = value[key]
if isinstance(value, bool):
    print("true" if value else "false")
elif value is not None:
    print(value)
' "$1" "$2"
}

fetch_json() {
    local path="$1"
    local output="$2"
    curl --silent --show-error --fail-with-body \
        --connect-timeout "$CONNECT_TIMEOUT_SECONDS" \
        --max-time 10 \
        --output "$output" \
        "$BASE_URL$path"
}

if ((OTA_PORT == 80)); then
    readonly BASE_URL="http://${DEVICE_HOST}"
else
    readonly BASE_URL="http://${DEVICE_HOST}:${OTA_PORT}"
fi
response_file="$(mktemp)"
status_file="$(mktemp)"
live_file="$(mktemp)"
image_info_file="$(mktemp)"
trap 'rm -f -- "$response_file" "$status_file" "$live_file" "$image_info_file"' EXIT

printf 'Inspecting local firmware image...\n'
if ! inspect_image >"$image_info_file"; then
    printf 'Could not inspect firmware image: %s\n' "$IMAGE" >&2
    exit 1
fi
require_image_metadata '^Checksum: 0x[[:xdigit:]]+ \(valid\)$' 'valid image checksum'
require_image_metadata '^Validation hash: [[:xdigit:]]{64} \(valid\)$' 'valid image hash'
require_image_metadata '^Project name: esp32-cc1101$' 'esp32-cc1101 project identity'
require_image_metadata '^ESP-IDF: v6\.0\.2$' 'ESP-IDF 6.0.2 metadata'
require_image_metadata '^App version: .{1,32}$' 'application version'
require_image_metadata '^ELF file SHA256: [[:xdigit:]]{64}$' 'ELF SHA256 identity'

image_version="$(sed -n 's/^App version: //p' "$image_info_file" | head -n 1)"
image_digest="$(sed -n 's/^ELF file SHA256: //p' "$image_info_file" | head -n 1 | tr '[:upper:]' '[:lower:]')"
readonly IMAGE_VERSION="$image_version"
readonly IMAGE_DIGEST="$image_digest"

descriptor_hex="$(od -An -v -j "$BOARD_DESCRIPTOR_OFFSET" -N 16 -tx1 "$IMAGE" | tr -d '[:space:]')"
case "$descriptor_hex" in
    52464244011001010401000000000000)
        image_profile="esp32-devkit"
        ota_slot_size=$((0x1e0000))
        expected_flash='Flash size: 4MB'
        expected_chip='Chip ID: 0 (ESP32)'
        ;;
    52464244011003020802000000000000)
        image_profile="xiao-esp32s3"
        ota_slot_size=$((0x3e0000))
        expected_flash='Flash size: 8MB'
        expected_chip='Chip ID: 9 (ESP32-S3)'
        ;;
    52464244011002021003000000000000)
        image_profile="esp32s3-devkitc-n16r8"
        ota_slot_size=$((0x7e0000))
        expected_flash='Flash size: 16MB'
        expected_chip='Chip ID: 9 (ESP32-S3)'
        ;;
    52464244011004020401000000000000)
        image_profile="esp32s3-supermini-fh4r2"
        ota_slot_size=$((0x1e0000))
        expected_flash='Flash size: 4MB'
        expected_chip='Chip ID: 9 (ESP32-S3)'
        ;;
    *)
        printf 'Image validation failed: missing or unsupported RFBD board descriptor\n' >&2
        exit 1
        ;;
esac
readonly IMAGE_PROFILE="$image_profile"
readonly OTA_SLOT_SIZE="$ota_slot_size"
require_exact_image_metadata "$expected_flash" "$IMAGE_PROFILE flash header"
require_exact_image_metadata "$expected_chip" "$IMAGE_PROFILE chip ID"
if ((IMAGE_SIZE > OTA_SLOT_SIZE)); then
    printf 'Firmware image size %s is outside the %s OTA slot limit (1..0x%x bytes)\n' \
        "$IMAGE_SIZE" "$IMAGE_PROFILE" "$OTA_SLOT_SIZE" >&2
    exit 2
fi
printf 'Image profile: %s; version: %s; ELF SHA256: %s\n' \
    "$IMAGE_PROFILE" "$IMAGE_VERSION" "$IMAGE_DIGEST"
printf 'OTA slot limit: 0x%x bytes\n' "$OTA_SLOT_SIZE"

printf 'Checking OTA service at %s...\n' "$BASE_URL"
fetch_json '/api/v1/ota/status' "$status_file"
fetch_json '/api/live' "$live_file"
if ! status_state="$(json_field "$status_file" state)" || \
   ! upload_active="$(json_field "$status_file" upload)" || \
   ! pending_verification="$(json_field "$status_file" pending_verification)"; then
    printf 'OTA preflight returned invalid JSON\n' >&2
    exit 1
fi
running_partition="$(json_field "$status_file" running_partition)"
target_partition="$(json_field "$status_file" update_partition)"
device_profile="$(json_field "$status_file" board_profile)"
if [[ -z "$device_profile" ]]; then
    device_profile="$(json_field "$live_file" board.profile)"
fi
requested_services="$(json_field "$live_file" services.requested)"
if [[ "$upload_active" == "true" || "$pending_verification" == "true" || \
      "$status_state" == "receiving" || "$status_state" == "validating" || \
      "$status_state" == "pending_reboot" ]]; then
    printf 'OTA preflight refused: device state is %s (upload=%s, pending_verification=%s)\n' \
        "${status_state:-unknown}" "${upload_active:-unknown}" \
        "${pending_verification:-unknown}" >&2
    exit 1
fi
if [[ -z "$device_profile" ]]; then
    printf 'OTA preflight refused: device board profile is unavailable\n' >&2
    exit 1
fi
if [[ "$device_profile" != "$IMAGE_PROFILE" ]]; then
    printf 'OTA preflight refused: image profile %s does not match device profile %s\n' \
        "$IMAGE_PROFILE" "$device_profile" >&2
    exit 1
fi
if [[ -z "$running_partition" || -z "$target_partition" || \
      "$running_partition" == "$target_partition" ]]; then
    printf 'OTA preflight refused: running and inactive partition identity is unavailable\n' >&2
    exit 1
fi
if [[ "$requested_services" == "mqtt" && "$wait_for_confirmation" == true ]]; then
    printf 'OTA preflight refused: the next boot is MQTT-only, so HTTP boot confirmation is unavailable.\n' >&2
    printf 'Select Web or Both and reboot first, or use --no-wait for acceptance-only behavior.\n' >&2
    exit 1
fi
printf 'Device profile: %s; running: %s; target: %s; next services: %s\n' \
    "$device_profile" "$running_partition" "$target_partition" \
    "${requested_services:-unknown}"

printf 'Uploading %s bytes from %s...\n' "$IMAGE_SIZE" "$IMAGE"
upload_progress=(--no-progress-meter)
if [[ -t 2 ]]; then
    upload_progress=()
fi
: >"$response_file"
if curl --show-error --fail-with-body \
    "${upload_progress[@]}" \
    --connect-timeout "$CONNECT_TIMEOUT_SECONDS" \
    --max-time "$UPLOAD_TIMEOUT_SECONDS" \
    --header 'Content-Type: application/octet-stream' \
    --header "Origin: $BASE_URL" \
    --header 'Expect:' \
    --data-binary "@$IMAGE" \
    --output "$response_file" \
    "$BASE_URL/api/v1/ota"; then
    :
else
    curl_status=$?
    printf '\nOTA upload failed' >&2
    if [[ -s "$response_file" ]]; then
        printf ': ' >&2
        tr -d '\r\n' <"$response_file" >&2
    fi
    printf '\n' >&2
    exit "$curl_status"
fi

if ! response_ok="$(json_field "$response_file" ok)" || [[ "$response_ok" != "true" ]]; then
    printf 'OTA service did not confirm acceptance: ' >&2
    tr -d '\r\n' <"$response_file" >&2
    printf '\n' >&2
    exit 1
fi
response_target="$(json_field "$response_file" target_partition)"
response_digest="$(json_field "$response_file" candidate_elf_sha256 | tr '[:upper:]' '[:lower:]')"
if [[ -n "$response_target" && "$response_target" != "$target_partition" ]]; then
    printf 'OTA response target %s does not match preflight target %s\n' \
        "$response_target" "$target_partition" >&2
    exit 1
fi
if [[ -n "$response_digest" && "$response_digest" != "$IMAGE_DIGEST" ]]; then
    printf 'OTA response ELF SHA256 does not match the uploaded image\n' >&2
    exit 1
fi
printf 'OTA accepted: '
tr -d '\r\n' <"$response_file"
printf '\n'
if [[ "$wait_for_confirmation" != true ]]; then
    printf 'Acceptance-only mode selected; boot and rollback outcome were not checked.\n'
    exit 0
fi

printf 'Waiting up to %s seconds for exact boot confirmation...\n' "$CONFIRM_TIMEOUT_SECONDS"
deadline=$((SECONDS + CONFIRM_TIMEOUT_SECONDS))
last_observation="device unavailable"
while ((SECONDS <= deadline)); do
    if fetch_json '/api/v1/ota/status' "$status_file" 2>/dev/null && \
       poll_partition="$(json_field "$status_file" running_partition 2>/dev/null)" && \
       poll_state="$(json_field "$status_file" state 2>/dev/null)"; then
        poll_digest="$(json_field "$status_file" running_elf_sha256 | tr '[:upper:]' '[:lower:]')"
        image_state="$(json_field "$status_file" running_image_state)"
        state_error="$(json_field "$status_file" running_image_state_error)"
        confirmation_error="$(json_field "$status_file" confirmation_error)"
        last_observation="partition=${poll_partition:-unknown}, state=${poll_state:-unknown}, image_state=${image_state:-unknown}, state_error=${state_error:-unknown}, confirmation_error=${confirmation_error:-unknown}"
        if [[ "$poll_partition" == "$target_partition" && \
              "$poll_digest" == "$IMAGE_DIGEST" && \
              "$image_state" == "valid" && "$state_error" == "ESP_OK" && \
              "$confirmation_error" == "ESP_OK" ]]; then
            printf 'OTA boot confirmed: partition %s, version %s, ELF SHA256 %s, image state valid.\n' \
                "$poll_partition" "$IMAGE_VERSION" "$IMAGE_DIGEST"
            exit 0
        fi
        if [[ "$poll_partition" == "$target_partition" && -n "$poll_digest" && \
              "$poll_digest" != "$IMAGE_DIGEST" ]]; then
            printf 'OTA confirmation failed: target partition booted with a different ELF SHA256.\n' >&2
            exit 1
        fi
        if [[ "$poll_partition" == "$target_partition" && \
              ( "$image_state" == "invalid" || "$image_state" == "aborted" || \
                ( -n "$state_error" && "$state_error" != "ESP_OK" ) || \
                ( -n "$confirmation_error" && "$confirmation_error" != "ESP_OK" && \
                  "$confirmation_error" != "ESP_ERR_INVALID_STATE" ) ) ]]; then
            printf 'OTA confirmation failed: target image validation failed (%s).\n' \
                "$last_observation" >&2
            exit 1
        fi
        if [[ "$poll_partition" == "$running_partition" && \
              "$poll_state" != "pending_reboot" ]]; then
            printf 'OTA confirmation failed: device returned to original partition %s (rollback or reboot rejection).\n' \
                "$running_partition" >&2
            exit 1
        fi
    fi
    sleep "$POLL_INTERVAL_SECONDS"
done

printf 'OTA was accepted but boot was not confirmed within %s seconds (%s).\n' \
    "$CONFIRM_TIMEOUT_SECONDS" "$last_observation" >&2
exit 1
