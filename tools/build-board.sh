#!/usr/bin/env bash

set -euo pipefail

readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
readonly PROJECT_ROOT="$(cd -- "$SCRIPT_DIR/.." && pwd -P)"
readonly IDF_EXPORT="$HOME/.espressif/v6.0.2/esp-idf/export.sh"
readonly CLASSIC_APPROVED_PORT="/dev/serial/by-id/usb-EXAMPLE_CLASSIC-if00"
readonly N16R8_APPROVED_PORT="/dev/serial/by-id/usb-EXAMPLE_N16R8-if00"
readonly SUPERMINI_FH4R2_APPROVED_PORT="/dev/serial/by-id/usb-EXAMPLE_SUPERMINI_FH4R2-if00"

usage() {
    printf 'usage: %s <esp32-devkit|esp32s3-devkitc-n16r8|xiao-esp32s3|esp32s3-supermini-fh4r2> <build|size|flash|monitor> [--port <path>]\n' "$0" >&2
    exit 2
}

[[ $# -ge 2 ]] || usage
profile="$1"
action="$2"
shift 2

asserted_port=""
if [[ $# -ne 0 ]]; then
    [[ $# -eq 2 && "$1" == "--port" ]] || usage
    asserted_port="$2"
fi

case "$profile" in
    esp32-devkit)
        target="esp32"
        approved_port="$CLASSIC_APPROVED_PORT"
        expected_flash_define="CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y"
        expected_flash_info="Flash size: 4MB"
        expected_chip_info="Chip ID: 0 (ESP32)"
        expected_profile_define="CONFIG_PLATFORM_BOARD_ESP32_DEVKIT=y"
        expected_descriptor='52464244 01100101 04010000 00000000'
        ;;
    esp32s3-devkitc-n16r8)
        target="esp32s3"
        approved_port="$N16R8_APPROVED_PORT"
        expected_flash_define="CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y"
        expected_flash_info="Flash size: 16MB"
        expected_chip_info="Chip ID: 9 (ESP32-S3)"
        expected_profile_define="CONFIG_PLATFORM_BOARD_ESP32S3_DEVKITC_N16R8=y"
        expected_descriptor='52464244 01100202 10030000 00000000'
        ;;
    xiao-esp32s3)
        target="esp32s3"
        approved_port=""
        expected_flash_define="CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y"
        expected_flash_info="Flash size: 8MB"
        expected_chip_info="Chip ID: 9 (ESP32-S3)"
        expected_profile_define="CONFIG_PLATFORM_BOARD_XIAO_ESP32S3=y"
        expected_descriptor='52464244 01100302 08020000 00000000'
        ;;
    esp32s3-supermini-fh4r2)
        target="esp32s3"
        approved_port="$SUPERMINI_FH4R2_APPROVED_PORT"
        expected_flash_define="CONFIG_ESPTOOLPY_FLASHSIZE_4MB=y"
        expected_flash_info="Flash size: 4MB"
        expected_chip_info="Chip ID: 9 (ESP32-S3)"
        expected_profile_define="CONFIG_PLATFORM_BOARD_ESP32S3_SUPERMINI_FH4R2=y"
        expected_descriptor='52464244 01100402 04010000 00000000'
        ;;
    *) usage ;;
esac

case "$action" in
    build|size) [[ -z "$asserted_port" ]] || usage ;;
    flash|monitor) ;;
    *) usage ;;
esac

readonly BUILD_DIR="$PROJECT_ROOT/build/$profile"
readonly GENERATED_SDKCONFIG="$BUILD_DIR/sdkconfig"
readonly IMAGE="$BUILD_DIR/esp32-cc1101.bin"
readonly ELF="$BUILD_DIR/esp32-cc1101.elf"
readonly TARGET_DEFAULTS="$PROJECT_ROOT/sdkconfig.defaults.$target"
readonly BOARD_DEFAULTS="$PROJECT_ROOT/boards/$profile/sdkconfig.defaults"
readonly LOCK_FILE="$PROJECT_ROOT/dependencies.lock.$target"
readonly SDKCONFIG_DEFAULTS="$PROJECT_ROOT/sdkconfig.defaults;$TARGET_DEFAULTS;$BOARD_DEFAULTS"

if [[ ! -f "$IDF_EXPORT" || ! -f "$TARGET_DEFAULTS" || ! -f "$BOARD_DEFAULTS" ||
      ! -f "$LOCK_FILE" ]]; then
    printf 'Required ESP-IDF export, profile defaults, or dependency lock is missing\n' >&2
    exit 1
fi

source "$IDF_EXPORT" >/dev/null
readonly IDF_PYTHON="$IDF_PYTHON_ENV_PATH/bin/python"
readonly IDF_CLI="$IDF_PATH/tools/idf.py"

idf_command() {
    "$IDF_PYTHON" "$IDF_CLI" -C "$PROJECT_ROOT" -B "$BUILD_DIR" \
        -DIDF_TARGET="$target" -DSDKCONFIG="$GENERATED_SDKCONFIG" \
        -DSDKCONFIG_DEFAULTS="$SDKCONFIG_DEFAULTS" \
        -DDEPENDENCIES_LOCK="$LOCK_FILE" "$@"
}

validate_built_profile() {
    [[ -f "$GENERATED_SDKCONFIG" && -f "$IMAGE" && -f "$ELF" ]] || {
        printf 'Profile %s has not produced a firmware image; run its build first\n' "$profile" >&2
        exit 1
    }
    grep -qx "CONFIG_IDF_TARGET_${target^^}=y" "$GENERATED_SDKCONFIG"
    grep -qx "$expected_flash_define" "$GENERATED_SDKCONFIG"
    grep -qx "$expected_profile_define" "$GENERATED_SDKCONFIG"

    image_info="$(esptool --chip "$target" image-info "$IMAGE")"
    grep -Fqx "$expected_flash_info" <<<"$image_info"
    grep -Fqx "$expected_chip_info" <<<"$image_info"
    descriptor_info="$(objdump -s -j .flash.appdesc "$ELF")"
    grep -Eq "$expected_descriptor" <<<"$descriptor_info"
    section_info="$(objdump -h "$ELF")"
    awk '$2 == ".flash.appdesc" && $3 == "00000110" { found = 1 } END { exit !found }' \
        <<<"$section_info"
}

validate_hardware_port() {
    if [[ -z "$approved_port" ]]; then
        printf 'Hardware access for profile %s is disabled until a persistent by-id path is explicitly approved\n' "$profile" >&2
        exit 1
    fi
    if [[ -n "$asserted_port" && "$asserted_port" != "$approved_port" ]]; then
        printf 'Port assertion does not match the approved path for %s\n' "$profile" >&2
        exit 1
    fi
    if [[ ! -L "$approved_port" ]]; then
        printf 'Approved serial symlink is absent: %s\n' "$approved_port" >&2
        exit 1
    fi
    resolved_port="$(readlink -f -- "$approved_port")"
    if [[ -z "$resolved_port" || ! -c "$resolved_port" ]]; then
        printf 'Approved serial path does not resolve to a character device: %s\n' "$approved_port" >&2
        exit 1
    fi
}

case "$action" in
    build)
        idf_command build
        ;;
    size)
        idf_command size
        ;;
    flash)
        validate_hardware_port
        idf_command build
        validate_built_profile
        idf_command -p "$approved_port" flash
        ;;
    monitor)
        validate_hardware_port
        validate_built_profile
        idf_command -p "$approved_port" monitor
        ;;
esac
