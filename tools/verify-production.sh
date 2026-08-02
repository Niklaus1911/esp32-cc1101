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
readonly PARTITION_INFO_LOG="$LOG_DIR/partition-info.log"
readonly IMAGE="$BUILD_DIR/esp32-cc1101.bin"
readonly PARTITION_BIN="$BUILD_DIR/partition_table/partition-table.bin"
readonly SDKCONFIG_HEADER="$BUILD_DIR/config/sdkconfig.h"
readonly PRODUCTION_SDKCONFIG="$LOG_DIR/sdkconfig"
readonly OTA_SLOT_SIZE=$((0x1e0000))
readonly OTA_MIN_FREE=$((OTA_SLOT_SIZE / 4))

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

require_log_pattern() {
    local log_file="$1"
    local pattern="$2"
    local description="$3"

    if ! grep -Eq -- "$pattern" "$log_file"; then
        printf 'Production verification failed: missing %s\n' "$description" >&2
        tail -n 80 "$log_file" >&2
        return 1
    fi
}

run_logged "ESP-IDF production build" "$BUILD_LOG" "$IDF_PYTHON" "$IDF_CLI" -B "$BUILD_DIR" \
    -DSDKCONFIG="$PRODUCTION_SDKCONFIG" build
run_logged "ESP-IDF size report" "$SIZE_LOG" "$IDF_PYTHON" "$IDF_CLI" -B "$BUILD_DIR" size

if [[ ! -f "$IMAGE" || ! -f "$PARTITION_BIN" || ! -f "$SDKCONFIG_HEADER" ]]; then
    printf 'Expected image, partition table, or generated sdkconfig was not produced\n' >&2
    exit 1
fi
if (( $(wc -c <"$IMAGE") > OTA_SLOT_SIZE - OTA_MIN_FREE )); then
    printf 'Application image violates the 25%% OTA-slot growth margin (slot=0x%x)\n' \
        "$OTA_SLOT_SIZE" >&2
    exit 1
fi

run_logged "ESP32 image inspection" "$IMAGE_INFO_LOG" esptool --chip esp32 image-info "$IMAGE"
run_logged "Partition table inspection" "$PARTITION_INFO_LOG" "$IDF_PYTHON" \
    "$IDF_PATH/components/partition_table/gen_esp32part.py" "$PARTITION_BIN"
require_log_pattern "$IMAGE_INFO_LOG" '^Flash size: 4MB$' '4 MB flash header'
require_log_pattern "$IMAGE_INFO_LOG" '^Chip ID: 0 \(ESP32\)$' 'classic ESP32 chip ID'
require_log_pattern "$IMAGE_INFO_LOG" '^Checksum: 0x[[:xdigit:]]+ \(valid\)$' 'valid image checksum'
require_log_pattern "$IMAGE_INFO_LOG" '^Validation hash: [[:xdigit:]]{64} \(valid\)$' 'valid SHA-256 image hash'
require_log_pattern "$IMAGE_INFO_LOG" '^Project name: esp32-cc1101$' 'project identity metadata'
require_log_pattern "$IMAGE_INFO_LOG" '^ESP-IDF: v6\.0\.2$' 'ESP-IDF 6.0.2 application metadata'
require_log_pattern "$PARTITION_INFO_LOG" '^nvs,data,nvs,0x9000,24K,$' 'preserved NVS partition'
require_log_pattern "$PARTITION_INFO_LOG" '^otadata,data,ota,0xf000,8K,$' 'OTA selection partition'
require_log_pattern "$PARTITION_INFO_LOG" '^phy_init,data,phy,0x11000,4K,$' 'PHY initialization partition'
require_log_pattern "$PARTITION_INFO_LOG" '^ota_0,app,ota_0,0x20000,1920K,$' 'first OTA application slot'
require_log_pattern "$PARTITION_INFO_LOG" '^ota_1,app,ota_1,0x200000,1920K,$' 'second OTA application slot'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE 1$' 'bootloader rollback configuration'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_LOG_COLORS 1$' 'application log colors'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_BOOTLOADER_LOG_COLORS 1$' 'bootloader log colors'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_RF_ACTIVITY_LED_ENABLE 1$' 'RF activity LED enabled'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_RF_ACTIVITY_LED_GPIO 2$' 'RF activity LED GPIO2 default'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_RF_ACTIVITY_LED_ACTIVE_HIGH 1$' 'RF activity LED active-high default'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_RF_ACTIVITY_LED_PULSE_MS 25$' 'RF activity LED pulse duration'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_OTA_HTTP_PORT 8032$' 'bounded LAN OTA HTTP port'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_OTA_HTTP_TASK_STACK_SIZE 10240$' 'bounded OTA HTTP task stack'

printf '\nProduction verification passed.\n\n'
printf 'Build summary:\n'
grep -E 'Bootloader binary size|\.bin binary size|Project build complete' "$BUILD_LOG" | tail -n 4 || true
printf '\nSize summary:\n'
grep -E 'Total image size|Used static DRAM|Used static IRAM|Flash Code' "$SIZE_LOG" | tail -n 8 || true
printf '\nImage metadata:\n'
grep -E 'Flash size:|Chip ID:|Checksum:|Validation hash:|ESP-IDF:|Minimal eFuse block revision:|Maximal eFuse block revision:|MMU page size:|Secure version:' "$IMAGE_INFO_LOG" || true
printf '\nPartition table:\n'
grep -E '^(nvs|otadata|phy_init|ota_0|ota_1),' "$PARTITION_INFO_LOG" || true
printf '\nFull logs:\n  %s\n  %s\n  %s\n  %s\n' "$BUILD_LOG" "$SIZE_LOG" "$IMAGE_INFO_LOG" \
    "$PARTITION_INFO_LOG"
