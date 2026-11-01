#!/usr/bin/env bash
# Package verified production builds into per-profile release assets.
#
#   tools/package-release.sh <version> <production-build-root> <output-dir>
#
# <production-build-root> is the tools/verify-production.sh output (one <profile>/build per
# board). Each profile gets a zip for wired installs (application, bootloader, partition
# table, initial OTA data, flash_args with flat names, checksums, license texts) and a bare
# application image for OTA; SHA256SUMS covers every asset. Every image must report <version>.

set -euo pipefail

readonly VERSION="${1:?usage: package-release.sh <version> <production-build-root> <output-dir>}"
readonly BUILDS="${2:?usage: package-release.sh <version> <production-build-root> <output-dir>}"
mkdir -p -- "${3:?usage: package-release.sh <version> <production-build-root> <output-dir>}"
readonly OUT="$(cd -- "$3" && pwd -P)"
readonly REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)"
readonly IDF_LICENSE="${IDF_PATH:?IDF_PATH must point at the ESP-IDF used for the build}/LICENSE"
readonly PROFILES=(esp32-devkit esp32s3-devkitc-n16r8 xiao-esp32s3 esp32s3-supermini-fh4r2)

[[ -f "$IDF_LICENSE" ]] || { printf 'ESP-IDF license not found: %s\n' "$IDF_LICENSE" >&2; exit 1; }
commit="$(git -C "$REPO" rev-parse HEAD)"
stage_root="$(mktemp -d)"
trap 'rm -rf -- "$stage_root"' EXIT

for profile in "${PROFILES[@]}"; do
    build="$BUILDS/$profile/build"
    name="rfbridge-$VERSION-$profile"
    stage="$stage_root/$name"
    mkdir -p -- "$stage/licenses"

    version="$(python3 -c 'import json, sys; print(json.load(open(sys.argv[1]))["project_version"])' \
        "$build/project_description.json")"
    if [[ "$version" != "$VERSION" ]]; then
        printf '%s was built as %s, expected %s\n' "$profile" "$version" "$VERSION" >&2
        exit 1
    fi

    # Rewrite flash_args with the flat file names used inside the bundle.
    {
        read -r options
        printf '%s\n' "$options"
        while read -r offset path; do
            case "$path" in
                bootloader/bootloader.bin) file="bootloader.bin" ;;
                partition_table/partition-table.bin) file="partition-table.bin" ;;
                ota_data_initial.bin) file="ota_data_initial.bin" ;;
                esp32-cc1101.bin) file="$name.bin" ;;
                *) printf 'Unexpected flash_args entry for %s: %s\n' "$profile" "$path" >&2; exit 1 ;;
            esac
            cp -- "$build/$path" "$stage/$file"
            printf '%s %s\n' "$offset" "$file"
        done
    } <"$build/flash_args" >"$stage/flash_args"
    grep -q " $name.bin\$" "$stage/flash_args" || {
        printf 'Application missing from %s flash_args\n' "$profile" >&2
        exit 1
    }

    cp -- "$REPO/LICENSE" "$stage/licenses/LICENSE-GPL-3.0.txt"
    cp -- "$REPO/NOTICE.md" "$stage/licenses/NOTICE.md"
    cp -- "$REPO/components/rf_codec/NOTICE.md" "$stage/licenses/rc-switch-NOTICE.md"
    cp -- "$REPO/components/rf_codec/COPYING.LESSER" "$stage/licenses/COPYING.LESSER-2.1.txt"
    cp -- "$IDF_LICENSE" "$stage/licenses/ESP-IDF-LICENSE.txt"
    cp -- "$REPO/managed_components/espressif__mdns/LICENSE" "$stage/licenses/espressif-mdns-LICENSE.txt"
    cp -- "$REPO/managed_components/espressif__mqtt/LICENSE" "$stage/licenses/espressif-mqtt-LICENSE.txt"

    cat >"$stage/README.txt" <<EOF
RFBridge $VERSION - $profile
Built from https://github.com/Niklaus1911/esp32-cc1101 tag $VERSION (commit $commit).

OTA update (device already running this profile): upload $name.bin from
Web UI > System > Firmware update, or with tools/push-ota.sh.

First wired install: from this folder run
  esptool --chip <esp32|esp32s3> --port <serial-port> write-flash @flash_args
This writes the bootloader, partition table, OTA data, and application only and
preserves NVS (0x9000, size 0x6000). Do not run erase-flash and do not merge these
files into a single image. Back up NVS first if the board holds data:
  esptool --chip <esp32|esp32s3> --port <serial-port> read-flash 0x9000 0x6000 nvs-backup.bin

Never install this image on a different board profile.
License: GPL-3.0-or-later; third-party notices are in licenses/.
EOF
    (cd -- "$stage" && sha256sum -- *.bin >SHA256SUMS)
    cp -- "$stage/$name.bin" "$OUT/$name.bin"
    (cd -- "$stage_root" && python3 -m zipfile -c "$OUT/$name.zip" "$name")
done

(cd -- "$OUT" && sha256sum -- *.zip *.bin >SHA256SUMS && cat SHA256SUMS)
