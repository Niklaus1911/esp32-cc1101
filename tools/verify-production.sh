#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
readonly IDF_EXPORT="$HOME/.espressif/v6.0.2/esp-idf/export.sh"
readonly OUTPUT_ROOT="/tmp/esp32-cc1101-production"
readonly MDNS_MANIFEST="$PROJECT_ROOT/components/network_mdns/idf_component.yml"
readonly CLASSIC_LOCK="$PROJECT_ROOT/dependencies.lock.esp32"
readonly S3_LOCK="$PROJECT_ROOT/dependencies.lock.esp32s3"

[[ -f "$IDF_EXPORT" ]] || { printf 'ESP-IDF export script not found: %s\n' "$IDF_EXPORT" >&2; exit 1; }
rm -rf -- "$OUTPUT_ROOT"
mkdir -p -- "$OUTPUT_ROOT"
source "$IDF_EXPORT" >/dev/null
readonly IDF_PYTHON="$IDF_PYTHON_ENV_PATH/bin/python"
readonly IDF_CLI="$IDF_PATH/tools/idf.py"
readonly PARTITION_TOOL="$IDF_PATH/components/partition_table/gen_esp32part.py"
cd -- "$PROJECT_ROOT"

require_exact_line() {
    local file="$1"
    local line="$2"
    local description="$3"
    if ! grep -Fqx -- "$line" "$file"; then
        printf 'Production verification failed: missing %s in %s\n' \
            "$description" "$file" >&2
        return 1
    fi
}

verify_dependency_lock() {
    local lock="$1"
    local target="$2"
    require_exact_line "$lock" '    version: 1.11.3' 'mDNS 1.11.3 lock version'
    require_exact_line "$lock" '- espressif/mdns' 'direct mDNS lock entry'
    require_exact_line "$lock" "target: $target" "dependency-lock target $target"
}

run_profile() {
    local profile="$1"
    local target="$2"
    local flash_mib="$3"
    local layout="$4"
    local slot_size="$5"
    local lock="$6"
    local descriptor_hex="$7"
    local build_dir="$OUTPUT_ROOT/$profile/build"
    local log_dir="$OUTPUT_ROOT/$profile/logs"
    local sdkconfig="$build_dir/sdkconfig"
    local image="$build_dir/esp32-cc1101.bin"
    local elf="$build_dir/esp32-cc1101.elf"
    local partition_bin="$build_dir/partition_table/partition-table.bin"
    local defaults="$PROJECT_ROOT/sdkconfig.defaults;$PROJECT_ROOT/sdkconfig.defaults.$target;$PROJECT_ROOT/boards/$profile/sdkconfig.defaults"
    mkdir -p -- "$log_dir"

    printf 'Building %s (%s, %s MiB flash)...\n' "$profile" "$target" "$flash_mib"
    "$IDF_PYTHON" "$IDF_CLI" -C "$PROJECT_ROOT" -B "$build_dir" \
        -DIDF_TARGET="$target" -DSDKCONFIG="$sdkconfig" \
        -DSDKCONFIG_DEFAULTS="$defaults" -DDEPENDENCIES_LOCK="$lock" \
        build >"$log_dir/build.log" 2>&1 || { tail -n 100 "$log_dir/build.log" >&2; return 1; }
    "$IDF_PYTHON" "$IDF_CLI" -C "$PROJECT_ROOT" -B "$build_dir" size >"$log_dir/size.log" 2>&1
    esptool --chip "$target" image-info "$image" >"$log_dir/image-info.log"
    "$IDF_PYTHON" "$PARTITION_TOOL" "$partition_bin" >"$log_dir/partition-info.log"

    [[ -f "$image" && -f "$elf" && -f "$partition_bin" && -f "$sdkconfig" ]] || {
        printf '%s did not produce expected build artifacts\n' "$profile" >&2; return 1;
    }
    local image_size
    image_size="$(wc -c <"$image")"
    local margin=$((slot_size / 4))
    (( image_size <= slot_size - margin )) || {
        printf '%s violates the 25%% OTA-slot margin\n' "$profile" >&2; return 1;
    }
    grep -Fqx "Flash size: ${flash_mib}MB" "$log_dir/image-info.log"
    if [[ "$target" == esp32 ]]; then
        grep -Fqx 'Chip ID: 0 (ESP32)' "$log_dir/image-info.log"
    else
        grep -Fqx 'Chip ID: 9 (ESP32-S3)' "$log_dir/image-info.log"
    fi
    grep -Eq '^Checksum: 0x[[:xdigit:]]+ \(valid\)$' "$log_dir/image-info.log"
    grep -Eq '^Validation hash: [[:xdigit:]]{64} \(valid\)$' "$log_dir/image-info.log"
    grep -Fqx 'Project name: esp32-cc1101' "$log_dir/image-info.log"
    grep -Fqx 'ESP-IDF: v6.0.2' "$log_dir/image-info.log"
    grep -Fqx 'nvs,data,nvs,0x9000,24K,' "$log_dir/partition-info.log"
    grep -Fqx 'otadata,data,ota,0xf000,8K,' "$log_dir/partition-info.log"
    grep -Fqx 'phy_init,data,phy,0x11000,4K,' "$log_dir/partition-info.log"
    case "$layout" in
        1) grep -Fqx 'ota_0,app,ota_0,0x20000,1920K,' "$log_dir/partition-info.log"; grep -Fqx 'ota_1,app,ota_1,0x200000,1920K,' "$log_dir/partition-info.log";;
        2) grep -Fqx 'ota_0,app,ota_0,0x20000,3968K,' "$log_dir/partition-info.log"; grep -Fqx 'ota_1,app,ota_1,0x400000,3968K,' "$log_dir/partition-info.log";;
        3) grep -Fqx 'ota_0,app,ota_0,0x20000,8064K,' "$log_dir/partition-info.log"; grep -Fqx 'ota_1,app,ota_1,0x800000,8064K,' "$log_dir/partition-info.log";;
    esac
    local second_slot_offset=$((layout == 1 ? 0x200000 : layout == 2 ? 0x400000 : 0x800000))
    local flash_bytes=$((flash_mib * 1024 * 1024))
    (( flash_bytes - second_slot_offset - slot_size == 0x20000 )) || {
        printf '%s does not preserve the required final 0x20000 flash reserve\n' "$profile" >&2
        return 1
    }
    objdump -s -j .flash.appdesc "$elf" >"$log_dir/descriptor.log"
    grep -Eq "$descriptor_hex" "$log_dir/descriptor.log"
    objdump -h "$elf" >"$log_dir/sections.log"
    awk '$2 == ".flash.appdesc" && $3 == "00000110" { found = 1 } END { exit !found }' \
        "$log_dir/sections.log"
    grep -Fqx "CONFIG_ESPTOOLPY_FLASHSIZE_${flash_mib}MB=y" "$sdkconfig"
    require_exact_line "$sdkconfig" 'CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y' \
        'bootloader rollback configuration'
    require_exact_line "$sdkconfig" 'CONFIG_LOG_COLORS=y' 'application log colors'
    require_exact_line "$sdkconfig" 'CONFIG_BOOTLOADER_LOG_COLORS=y' \
        'bootloader log colors'
    require_exact_line "$sdkconfig" 'CONFIG_OTA_HTTP_PORT=80' 'shared Web and OTA HTTP port'
    require_exact_line "$sdkconfig" 'CONFIG_OTA_HTTP_TASK_STACK_SIZE=8192' \
        'bounded OTA HTTP task stack'
    require_exact_line "$sdkconfig" 'CONFIG_MDNS_MAX_INTERFACES=2' \
        'mDNS duplicate-interface compatibility bound'
    require_exact_line "$sdkconfig" 'CONFIG_MDNS_MAX_SERVICES=2' 'mDNS service bound'
    require_exact_line "$sdkconfig" 'CONFIG_MDNS_ACTION_QUEUE_LEN=8' 'mDNS action queue bound'
    require_exact_line "$sdkconfig" 'CONFIG_MDNS_TASK_STACK_SIZE=4096' 'mDNS task stack bound'
    require_exact_line "$sdkconfig" 'CONFIG_MDNS_PREDEF_NETIF_STA=y' 'mDNS STA interface'
    require_exact_line "$sdkconfig" '# CONFIG_MDNS_PREDEF_NETIF_AP is not set' \
        'disabled mDNS AP interface'
    require_exact_line "$sdkconfig" '# CONFIG_MDNS_PREDEF_NETIF_ETH is not set' \
        'disabled mDNS Ethernet interface'
    require_exact_line "$sdkconfig" '# CONFIG_MDNS_ENABLE_CONSOLE_CLI is not set' \
        'disabled mDNS console CLI'
    require_exact_line "$sdkconfig" '# CONFIG_MDNS_MULTIPLE_INSTANCE is not set' \
        'disabled mDNS multiple-instance support'
    if [[ "$target" == esp32s3 ]]; then
        require_exact_line "$sdkconfig" 'CONFIG_ESPTOOLPY_FLASHMODE_DIO=y' \
            'S3 DIO flash mode'
        require_exact_line "$sdkconfig" 'CONFIG_ESPTOOLPY_FLASHFREQ_40M=y' \
            'S3 40 MHz flash frequency'
        require_exact_line "$sdkconfig" 'CONFIG_SPIRAM=y' 'required S3 PSRAM'
        case "$profile" in
            esp32s3-supermini-fh4r2)
                require_exact_line "$sdkconfig" 'CONFIG_SPIRAM_MODE_QUAD=y' 'Quad FH4R2 PSRAM'
                require_exact_line "$sdkconfig" '# CONFIG_SPIRAM_MODE_OCT is not set' \
                    'disabled FH4R2 Octal PSRAM';;
            *)
                require_exact_line "$sdkconfig" 'CONFIG_SPIRAM_MODE_OCT=y' 'Octal S3 PSRAM'
                require_exact_line "$sdkconfig" '# CONFIG_SPIRAM_MODE_QUAD is not set' \
                    'disabled S3 Quad PSRAM';;
        esac
        require_exact_line "$sdkconfig" 'CONFIG_SPIRAM_SPEED_80M=y' '80 MHz S3 PSRAM'
        require_exact_line "$sdkconfig" 'CONFIG_SPIRAM_BOOT_HW_INIT=y' \
            'S3 boot PSRAM initialization'
        require_exact_line "$sdkconfig" 'CONFIG_SPIRAM_BOOT_INIT=y' \
            'S3 application PSRAM initialization'
        require_exact_line "$sdkconfig" '# CONFIG_SPIRAM_IGNORE_NOTFOUND is not set' \
            'required S3 PSRAM detection'
        require_exact_line "$sdkconfig" 'CONFIG_SPIRAM_MEMTEST=y' 'S3 boot PSRAM memory test'
        require_exact_line "$sdkconfig" 'CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y' \
            'Wi-Fi/lwIP PSRAM allocation'
        require_exact_line "$sdkconfig" 'CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=65536' \
            '64 KiB internal-memory reserve'
        require_exact_line "$sdkconfig" 'CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384' \
            '16 KiB normal internal-allocation threshold'
        require_exact_line "$sdkconfig" '# CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM is not set' \
            'internal-only task stacks'
    fi
    case "$profile" in
        esp32-devkit)
            require_exact_line "$sdkconfig" 'CONFIG_PLATFORM_BOARD_ESP32_DEVKIT=y' 'classic board profile';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_SCLK_GPIO=18' 'classic CC1101 SCK';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_MISO_GPIO=19' 'classic CC1101 MISO';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_MOSI_GPIO=23' 'classic CC1101 MOSI';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_CS_GPIO=27' 'classic CC1101 CSN';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_GDO0_GPIO=26' 'classic CC1101 GDO0';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_GDO2_GPIO=25' 'classic CC1101 GDO2';
            require_exact_line "$sdkconfig" 'CONFIG_RF_ACTIVITY_LED_ENABLE=y' 'classic activity LED';
            require_exact_line "$sdkconfig" 'CONFIG_RF_ACTIVITY_LED_GPIO=2' 'classic activity LED GPIO';
            require_exact_line "$sdkconfig" 'CONFIG_RF_ACTIVITY_LED_ACTIVE_HIGH=y' 'classic active-high LED';
            require_exact_line "$sdkconfig" 'CONFIG_RF_ACTIVITY_LED_PULSE_MS=25' 'classic LED pulse duration';
            require_exact_line "$sdkconfig" 'CONFIG_ESP_CONSOLE_UART_DEFAULT=y' 'classic UART console';
            require_exact_line "$sdkconfig" 'CONFIG_ESP_CONSOLE_UART_NUM=0' 'classic UART0 console';;
        esp32s3-devkitc-n16r8)
            require_exact_line "$sdkconfig" 'CONFIG_PLATFORM_BOARD_ESP32S3_DEVKITC_N16R8=y' 'N16R8 board profile';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_SCLK_GPIO=12' 'N16R8 CC1101 SCK';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_MISO_GPIO=13' 'N16R8 CC1101 MISO';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_MOSI_GPIO=11' 'N16R8 CC1101 MOSI';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_CS_GPIO=10' 'N16R8 CC1101 CSN';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_GDO0_GPIO=4' 'N16R8 CC1101 GDO0';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_GDO2_GPIO=5' 'N16R8 CC1101 GDO2';
            require_exact_line "$sdkconfig" '# CONFIG_RF_ACTIVITY_LED_ENABLE is not set' 'disabled N16R8 LED';
            require_exact_line "$sdkconfig" 'CONFIG_ESP_CONSOLE_UART_DEFAULT=y' 'N16R8 UART console';
            require_exact_line "$sdkconfig" 'CONFIG_ESP_CONSOLE_UART_NUM=0' 'N16R8 UART0 console';
            require_exact_line "$sdkconfig" '# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set' 'disabled N16R8 native USB console';;
        xiao-esp32s3)
            require_exact_line "$sdkconfig" 'CONFIG_PLATFORM_BOARD_XIAO_ESP32S3=y' 'XIAO board profile';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_SCLK_GPIO=7' 'XIAO CC1101 SCK';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_MISO_GPIO=8' 'XIAO CC1101 MISO';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_MOSI_GPIO=9' 'XIAO CC1101 MOSI';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_CS_GPIO=4' 'XIAO CC1101 CSN';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_GDO0_GPIO=2' 'XIAO CC1101 GDO0';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_GDO2_GPIO=1' 'XIAO CC1101 GDO2';
            require_exact_line "$sdkconfig" 'CONFIG_RF_ACTIVITY_LED_ENABLE=y' 'XIAO activity LED';
            require_exact_line "$sdkconfig" 'CONFIG_RF_ACTIVITY_LED_GPIO=21' 'XIAO activity LED GPIO';
            require_exact_line "$sdkconfig" '# CONFIG_RF_ACTIVITY_LED_ACTIVE_HIGH is not set' 'XIAO active-low LED';
            require_exact_line "$sdkconfig" 'CONFIG_RF_ACTIVITY_LED_PULSE_MS=25' 'XIAO LED pulse duration';
            require_exact_line "$sdkconfig" 'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y' 'XIAO native USB console';
            require_exact_line "$sdkconfig" 'CONFIG_ESP_CONSOLE_UART_NUM=-1' 'disabled XIAO UART console';;
        esp32s3-supermini-fh4r2)
            require_exact_line "$sdkconfig" 'CONFIG_PLATFORM_BOARD_ESP32S3_SUPERMINI_FH4R2=y' \
                'FH4R2 board profile';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_SCLK_GPIO=12' 'FH4R2 CC1101 SCK';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_MISO_GPIO=13' 'FH4R2 CC1101 MISO';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_MOSI_GPIO=11' 'FH4R2 CC1101 MOSI';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_SPI_CS_GPIO=10' 'FH4R2 CC1101 CSN';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_GDO0_GPIO=4' 'FH4R2 CC1101 GDO0';
            require_exact_line "$sdkconfig" 'CONFIG_CC1101_GDO2_GPIO=5' 'FH4R2 CC1101 GDO2';
            require_exact_line "$sdkconfig" '# CONFIG_RF_ACTIVITY_LED_ENABLE is not set' \
                'disabled FH4R2 LED';
            require_exact_line "$sdkconfig" 'CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y' \
                'FH4R2 native USB console';
            require_exact_line "$sdkconfig" 'CONFIG_ESP_CONSOLE_UART_NUM=-1' \
                'disabled FH4R2 UART console';;
    esac
    printf '%s passed: image=0x%x bytes, slot=0x%x, free_margin=0x%x\n' "$profile" "$image_size" "$slot_size" "$((slot_size - image_size))"
}

[[ -f "$MDNS_MANIFEST" && -f "$CLASSIC_LOCK" && -f "$S3_LOCK" ]] || {
    printf 'Dependency metadata is incomplete\n' >&2; exit 1;
}
grep -Fqx '  espressif/mdns: "1.11.3"' "$MDNS_MANIFEST"
verify_dependency_lock "$CLASSIC_LOCK" esp32
verify_dependency_lock "$S3_LOCK" esp32s3
require_exact_line "$PROJECT_ROOT/components/web_ui/web_ui.cpp" \
    'constexpr uint16_t kHttpPort = CONFIG_OTA_HTTP_PORT;' 'configured shared Web HTTP port'
require_exact_line "$PROJECT_ROOT/components/web_ui/web_ui.cpp" \
    'constexpr uint32_t kHttpTaskStackSize = CONFIG_OTA_HTTP_TASK_STACK_SIZE;' \
    'configured bounded Web HTTP task stack'
run_profile esp32-devkit esp32 4 1 $((0x1e0000)) "$CLASSIC_LOCK" \
    '52464244 01100101 04010000 00000000'
run_profile xiao-esp32s3 esp32s3 8 2 $((0x3e0000)) "$S3_LOCK" \
    '52464244 01100302 08020000 00000000'
run_profile esp32s3-devkitc-n16r8 esp32s3 16 3 $((0x7e0000)) "$S3_LOCK" \
    '52464244 01100202 10030000 00000000'
run_profile esp32s3-supermini-fh4r2 esp32s3 4 1 $((0x1e0000)) "$S3_LOCK" \
    '52464244 01100402 04010000 00000000'
printf '\nFour-profile production verification passed. Logs: %s\n' "$OUTPUT_ROOT"
