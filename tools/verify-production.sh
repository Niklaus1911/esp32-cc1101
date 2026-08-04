#!/usr/bin/env bash

set -eo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
readonly IDF_EXPORT="$HOME/.espressif/v6.0.2/esp-idf/export.sh"
readonly BUILD_DIR="/tmp/esp32-cc1101-production-build"
readonly LOG_DIR="/tmp/esp32-cc1101-production-logs"
readonly BUILD_LOG="$LOG_DIR/build.log"
readonly SIZE_LOG="$LOG_DIR/size.log"
readonly IMAGE_INFO_LOG="$LOG_DIR/image-info.log"
readonly PARTITION_INFO_LOG="$LOG_DIR/partition-info.log"
readonly MDNS_MANIFEST="$PROJECT_ROOT/components/network_mdns/idf_component.yml"
readonly DEPENDENCY_LOCK="$PROJECT_ROOT/dependencies.lock"
readonly IMAGE="$BUILD_DIR/esp32-cc1101.bin"
readonly PARTITION_BIN="$BUILD_DIR/partition_table/partition-table.bin"
readonly SDKCONFIG_HEADER="$BUILD_DIR/config/sdkconfig.h"
readonly PRODUCTION_SDKCONFIG="$LOG_DIR/sdkconfig"
readonly OTA_SLOT_SIZE=$((0x1e0000))
readonly OTA_MIN_FREE=$((OTA_SLOT_SIZE / 4))

if [[ ! -f "$IDF_EXPORT" ]]; then
    printf 'ESP-IDF export script not found: %s\n' "$IDF_EXPORT" >&2
    exit 1
fi

rm -rf -- "$BUILD_DIR" "$LOG_DIR"
mkdir -p -- "$LOG_DIR"

# Source the stable ESP-IDF checkout export; generated EIM activation helpers are not required.
source "$IDF_EXPORT" >/dev/null

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

if [[ ! -f "$IMAGE" || ! -f "$PARTITION_BIN" || ! -f "$SDKCONFIG_HEADER" ||
      ! -f "$MDNS_MANIFEST" || ! -f "$DEPENDENCY_LOCK" ]]; then
    printf 'Expected image, partition table, generated sdkconfig, or dependency metadata was not produced\n' >&2
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
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_OTA_HTTP_PORT 80$' 'shared Web and OTA HTTP port'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_OTA_HTTP_TASK_STACK_SIZE 8192$' 'bounded OTA HTTP task stack contract'
require_log_pattern "$MDNS_MANIFEST" '^  espressif/mdns: "1\.11\.3"$' 'exact mDNS manifest dependency'
require_log_pattern "$DEPENDENCY_LOCK" '^    version: 1\.11\.3$' 'mDNS 1.11.3 dependency lock'
require_log_pattern "$DEPENDENCY_LOCK" '^- espressif/mdns$' 'direct mDNS dependency lock entry'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_MDNS_MAX_INTERFACES 2$' 'mDNS two-slot 1.11.3 compatibility bound'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_MDNS_MAX_SERVICES 2$' 'mDNS service bound'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_MDNS_ACTION_QUEUE_LEN 8$' 'mDNS action queue bound'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_MDNS_TASK_STACK_SIZE 4096$' 'mDNS task stack bound'
require_log_pattern "$SDKCONFIG_HEADER" '^#define CONFIG_MDNS_PREDEF_NETIF_STA 1$' 'mDNS STA interface enabled'
if grep -Eq '^#define CONFIG_MDNS_(PREDEF_NETIF_AP|PREDEF_NETIF_ETH|ENABLE_CONSOLE_CLI|MULTIPLE_INSTANCE) 1$' \
        "$SDKCONFIG_HEADER"; then
    printf 'Production verification failed: an unsupported mDNS interface or feature is enabled\n' >&2
    exit 1
fi
require_log_pattern "$PROJECT_ROOT/components/web_ui/web_ui.cpp" '^constexpr uint16_t kHttpPort = 80;$' 'fixed responsive Web HTTP port'
require_log_pattern "$PROJECT_ROOT/components/web_ui/web_ui.cpp" '^constexpr uint32_t kHttpTaskStackSize = 8192;$' 'bounded responsive Web HTTP task stack'

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
