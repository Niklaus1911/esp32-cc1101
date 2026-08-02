#!/usr/bin/env bash

set -euo pipefail

readonly OTA_PORT="${OTA_HTTP_PORT:-8032}"
readonly IDF_ACTIVATE="$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
readonly CONNECT_TIMEOUT_SECONDS=5
readonly UPLOAD_TIMEOUT_SECONDS=900

usage() {
    printf 'Usage: %s <esp32-ipv4> <application-image.bin>\n' "${0##*/}" >&2
}

if [[ $# -ne 2 ]]; then
    usage
    exit 2
fi

readonly DEVICE_IP="$1"
readonly IMAGE="$2"

IFS='.' read -r -a octets <<<"$DEVICE_IP"
if [[ ${#octets[@]} -ne 4 ]]; then
    printf 'Invalid ESP32 IPv4 address: %s\n' "$DEVICE_IP" >&2
    exit 2
fi
for octet in "${octets[@]}"; do
    if [[ ! "$octet" =~ ^[0-9]{1,3}$ ]] || ((10#$octet > 255)); then
        printf 'Invalid ESP32 IPv4 address: %s\n' "$DEVICE_IP" >&2
        exit 2
    fi
done
if [[ ! "$OTA_PORT" =~ ^[0-9]+$ ]] || ((OTA_PORT < 1024 || OTA_PORT > 65535)); then
    printf 'Invalid OTA_HTTP_PORT: %s\n' "$OTA_PORT" >&2
    exit 2
fi
if [[ ! -f "$IMAGE" || ! -r "$IMAGE" ]]; then
    printf 'Firmware image is not a readable regular file: %s\n' "$IMAGE" >&2
    exit 2
fi
readonly IMAGE_SIZE="$(wc -c <"$IMAGE")"
if ((IMAGE_SIZE == 0 || IMAGE_SIZE > 0x1e0000)); then
    printf 'Firmware image size %s is outside the OTA slot limit (1..0x1e0000 bytes)\n' \
        "$IMAGE_SIZE" >&2
    exit 2
fi
if ! command -v curl >/dev/null 2>&1; then
    printf 'curl is required\n' >&2
    exit 1
fi

inspect_image() {
    if command -v esptool >/dev/null 2>&1; then
        esptool --chip esp32 image-info "$IMAGE"
    elif [[ -f "$IDF_ACTIVATE" ]]; then
        bash -c 'set +u; source "$1" >/dev/null; set -u; "$IDF_PYTHON_ENV_PATH/bin/python" -m esptool --chip esp32 image-info "$2"' \
            bash "$IDF_ACTIVATE" "$IMAGE"
    else
        printf 'esptool is unavailable and ESP-IDF 6.0.2 cannot be activated\n' >&2
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

readonly BASE_URL="http://${DEVICE_IP}:${OTA_PORT}"
response_file="$(mktemp)"
image_info_file="$(mktemp)"
trap 'rm -f -- "$response_file" "$image_info_file"' EXIT

printf 'Inspecting local ESP32 image...\n'
if ! inspect_image >"$image_info_file"; then
    printf 'Could not inspect firmware image: %s\n' "$IMAGE" >&2
    exit 1
fi
require_image_metadata '^Flash size: 4MB$' '4 MB flash header'
require_image_metadata '^Chip ID: 0 \(ESP32\)$' 'classic ESP32 chip ID'
require_image_metadata '^Checksum: 0x[[:xdigit:]]+ \(valid\)$' 'valid image checksum'
require_image_metadata '^Validation hash: [[:xdigit:]]{64} \(valid\)$' 'valid image hash'
require_image_metadata '^Project name: esp32-cc1101$' 'esp32-cc1101 project identity'
require_image_metadata '^ESP-IDF: v6\.0\.2$' 'ESP-IDF 6.0.2 metadata'

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
printf '\nThe ESP32 will reboot into the validated inactive slot.\n'
