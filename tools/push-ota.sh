#!/usr/bin/env bash

set -euo pipefail

readonly OTA_PORT="${OTA_HTTP_PORT:-80}"
readonly IDF_EXPORT="$HOME/.espressif/v6.0.2/esp-idf/export.sh"
readonly CONNECT_TIMEOUT_SECONDS=5
readonly UPLOAD_TIMEOUT_SECONDS=900
readonly BOARD_DESCRIPTOR_OFFSET=$((24 + 8 + 256))

usage() {
    printf 'Usage: %s <esp32-ipv4-or-hostname.local> <application-image.bin>\n' "${0##*/}" >&2
}

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
if ! command -v curl >/dev/null 2>&1; then
    printf 'curl is required\n' >&2
    exit 1
fi
if ! command -v od >/dev/null 2>&1; then
    printf 'od is required\n' >&2
    exit 1
fi

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

if ((OTA_PORT == 80)); then
    readonly BASE_URL="http://${DEVICE_HOST}"
else
    readonly BASE_URL="http://${DEVICE_HOST}:${OTA_PORT}"
fi
response_file="$(mktemp)"
image_info_file="$(mktemp)"
trap 'rm -f -- "$response_file" "$image_info_file"' EXIT

printf 'Inspecting local firmware image...\n'
if ! inspect_image >"$image_info_file"; then
    printf 'Could not inspect firmware image: %s\n' "$IMAGE" >&2
    exit 1
fi
require_image_metadata '^Checksum: 0x[[:xdigit:]]+ \(valid\)$' 'valid image checksum'
require_image_metadata '^Validation hash: [[:xdigit:]]{64} \(valid\)$' 'valid image hash'
require_image_metadata '^Project name: esp32-cc1101$' 'esp32-cc1101 project identity'
require_image_metadata '^ESP-IDF: v6\.0\.2$' 'ESP-IDF 6.0.2 metadata'

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
printf 'Image profile: %s; OTA slot limit: 0x%x bytes\n' "$IMAGE_PROFILE" "$OTA_SLOT_SIZE"

printf 'Checking OTA service at %s...\n' "$BASE_URL"
curl --silent --show-error --fail-with-body \
    --connect-timeout "$CONNECT_TIMEOUT_SECONDS" \
    --max-time 10 \
    --output "$response_file" \
    "$BASE_URL/api/v1/ota/status"
printf 'Device status: '
tr -d '\r\n' <"$response_file"
printf '\nUploading %s bytes from %s...\n' "$IMAGE_SIZE" "$IMAGE"
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

if ! grep -Eq '"ok"[[:space:]]*:[[:space:]]*true' "$response_file"; then
    printf 'OTA service did not confirm success: ' >&2
    tr -d '\r\n' <"$response_file" >&2
    printf '\n' >&2
    exit 1
fi
printf 'OTA accepted: '
tr -d '\r\n' <"$response_file"
printf '\nThe device will reboot into the validated inactive slot.\n'
