#!/usr/bin/env bash

set -eo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
readonly IDF_ACTIVATE="$HOME/.espressif/tools/activate_idf_v6.0.2.sh"
readonly BUILD_DIR="/tmp/esp32-cc1101-production-build"
readonly LOG_DIR="/tmp/esp32-cc1101-production-logs"
readonly BUILD_LOG="$LOG_DIR/build.log"
readonly SIZE_LOG="$LOG_DIR/size.log"
readonly IMAGE_INFO_LOG="$LOG_DIR/image-info.log"
readonly IMAGE="$BUILD_DIR/esp32-cc1101.bin"

if [[ ! -f "$IDF_ACTIVATE" ]]; then
    printf 'ESP-IDF activation script not found: %s\n' "$IDF_ACTIVATE" >&2
    exit 1
fi

rm -rf -- "$BUILD_DIR" "$LOG_DIR"
mkdir -p -- "$LOG_DIR"

# The activation helper's -e mode emits newline-delimited NAME=value records.
activation_env="$("$IDF_ACTIVATE" -e)"
idf_path=""
system_path=""
while IFS='=' read -r name value; do
    case "$name" in
        PATH)
            idf_path="$value"
            ;;
        SYSTEM_PATH)
            system_path="$value"
            ;;
        *)
            if [[ ! "$name" =~ ^[A-Z][A-Z0-9_]*$ ]]; then
                printf 'Invalid variable from ESP-IDF activation: %s\n' "$name" >&2
                exit 1
            fi
            printf -v "$name" '%s' "$value"
            export "$name"
            ;;
    esac
done <<<"$activation_env"

if [[ -z "$idf_path" || -z "$system_path" ]]; then
    printf 'ESP-IDF activation did not provide PATH and SYSTEM_PATH\n' >&2
    exit 1
fi

PATH="$idf_path:$system_path"
export PATH
unset activation_env idf_path system_path name value

readonly IDF_PYTHON="$IDF_PYTHON_ENV_PATH/bin/python"
readonly IDF_CLI="$IDF_PATH/tools/idf.py"
if [[ ! -x "$IDF_PYTHON" || ! -f "$IDF_CLI" ]]; then
    printf 'ESP-IDF Python or idf.py is unavailable after activation\n' >&2
    exit 1
fi
set -u

cd -- "$PROJECT_ROOT"

run_logged() {
    local label="$1"
    local log_file="$2"
    shift 2

    printf '%s...\n' "$label"
    if ! "$@" >"$log_file" 2>&1; then
        printf '%s failed. Last 80 log lines:\n' "$label" >&2
        tail -n 80 "$log_file" >&2
        return 1
    fi
}

require_image_metadata() {
    local pattern="$1"
    local description="$2"

    if ! grep -Eq -- "$pattern" "$IMAGE_INFO_LOG"; then
        printf 'Image verification failed: missing %s\n' "$description" >&2
        tail -n 80 "$IMAGE_INFO_LOG" >&2
        return 1
    fi
}

run_logged "ESP-IDF production build" "$BUILD_LOG" "$IDF_PYTHON" "$IDF_CLI" -B "$BUILD_DIR" build
run_logged "ESP-IDF size report" "$SIZE_LOG" "$IDF_PYTHON" "$IDF_CLI" -B "$BUILD_DIR" size

if [[ ! -f "$IMAGE" ]]; then
    printf 'Expected firmware image was not produced: %s\n' "$IMAGE" >&2
    exit 1
fi

run_logged "ESP32 image inspection" "$IMAGE_INFO_LOG" esptool --chip esp32 image-info "$IMAGE"
require_image_metadata '^Flash size: 4MB$' '4 MB flash header'
require_image_metadata '^Chip ID: 0 \(ESP32\)$' 'classic ESP32 chip ID'
require_image_metadata '^Checksum: 0x[[:xdigit:]]+ \(valid\)$' 'valid image checksum'
require_image_metadata '^Validation hash: [[:xdigit:]]{64} \(valid\)$' 'valid SHA-256 image hash'
require_image_metadata '^ESP-IDF: v6\.0\.2$' 'ESP-IDF 6.0.2 application metadata'

printf '\nProduction verification passed.\n\n'
printf 'Build summary:\n'
grep -E 'Bootloader binary size|\.bin binary size|Project build complete' "$BUILD_LOG" | tail -n 4 || true
printf '\nSize summary:\n'
grep -E 'Total image size|Used static DRAM|Used static IRAM|Flash Code' "$SIZE_LOG" | tail -n 8 || true
printf '\nImage metadata:\n'
grep -E 'Flash size:|Chip ID:|Checksum:|Validation hash:|ESP-IDF:|Minimal eFuse block revision:|Maximal eFuse block revision:|MMU page size:|Secure version:' "$IMAGE_INFO_LOG" || true
printf '\nFull logs:\n  %s\n  %s\n  %s\n' "$BUILD_LOG" "$SIZE_LOG" "$IMAGE_INFO_LOG"
