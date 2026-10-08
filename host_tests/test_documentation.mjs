import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { join } from "node:path";

const root = process.argv[2];
assert(root, "project root argument is required");

const read = (...parts) => readFileSync(join(root, ...parts), "utf8");
const compact = (value) => value.replace(/\s+/g, " ");
const readme = read("README.md");
const tooling = read("docs", "development-tooling-setup.md");
const policy = read("components", "platform_board", "platform_board_policy.cpp");
const buildScript = read("tools", "build-board.sh");
const otaScript = read("tools", "push-ota.sh");

const profiles = [
  {
    name: "esp32-devkit",
    cc: [18, 19, 23, 27, 26, 25],
    generic: [32, 33],
    slot: "0x1e0000",
    secondOffset: "0x200000",
    policyConsole: "ConsoleTransport::kUart0",
    policyActivity: ".activity_led_gpio = 2",
    readmeProfile: "| `esp32-devkit` | ESP32 | 4 MB / none | UART0 GPIO1/3 | GPIO2 active-high | `web`, `mqtt` |",
    readmePins: "| `esp32-devkit` | `18/19/23/27/26/25` | `32/33` |",
    toolingFacts: "4 MB / none | UART0 GPIO1/3 / GPIO2 active-high",
  },
  {
    name: "esp32s3-devkitc-n16r8",
    cc: [12, 13, 11, 10, 4, 5],
    generic: [13, 4],
    slot: "0x7e0000",
    secondOffset: "0x800000",
    policyConsole: "ConsoleTransport::kUart0",
    policyActivity: ".activity_led_gpio = -1",
    readmeProfile: "| `esp32s3-devkitc-n16r8` | ESP32-S3 | 16 MB / 8 MB Octal | UART0 GPIO43/44 through USB-UART | Disabled by default; GPIO48 reserved | `web`, `mqtt`, `both` |",
    readmePins: "| `esp32s3-devkitc-n16r8` | `12/13/11/10/4/5` | `13/4` |",
    toolingFacts: "16 MB / 8 MB Octal | UART0 GPIO43/44 / disabled, GPIO48 reserved",
  },
  {
    name: "xiao-esp32s3",
    cc: [7, 8, 9, 4, 2, 1],
    generic: [5, 6],
    slot: "0x3e0000",
    secondOffset: "0x400000",
    policyConsole: "ConsoleTransport::kUsbSerialJtag",
    policyActivity: ".activity_led_gpio = 21",
    readmeProfile: "| `xiao-esp32s3` | ESP32-S3 | 8 MB / 8 MB Octal | Native USB Serial/JTAG | GPIO21 active-low | `web`, `mqtt`, `both` |",
    readmePins: "| `xiao-esp32s3` | `D8/GPIO7, D9/GPIO8, D10/GPIO9, D3/GPIO4, D1/GPIO2, D0/GPIO1` | `D4/GPIO5, D5/GPIO6` |",
    toolingFacts: "8 MB / 8 MB Octal | USB Serial/JTAG / GPIO21 active-low",
  },
  {
    name: "esp32s3-supermini-fh4r2",
    cc: [12, 13, 11, 10, 4, 5],
    generic: [6, 7],
    slot: "0x1e0000",
    secondOffset: "0x200000",
    policyConsole: "ConsoleTransport::kUsbSerialJtag",
    policyActivity: ".activity_led_gpio = -1",
    readmeProfile: "| `esp32s3-supermini-fh4r2` | ESP32-S3FH4R2 | 4 MB / 2 MB Quad | Native USB Serial/JTAG | Disabled; GPIO48 reserved | `web`, `mqtt`, `both` |",
    readmePins: "| `esp32s3-supermini-fh4r2` | `12/13/11/10/4/5` | `6/7` |",
    toolingFacts: "4 MB / 2 MB Quad | USB Serial/JTAG / disabled, GPIO48 reserved",
  },
];

function boardPolicyBlock(profile) {
  const profileOffset = policy.indexOf(`.profile_name = "${profile}"`);
  const start = policy.lastIndexOf("    {", profileOffset);
  const end = policy.indexOf(".activity_led_active_high", profileOffset);
  assert(start >= 0 && end > start, `board policy block missing: ${profile}`);
  return compact(policy.slice(start, end));
}

for (const profile of profiles) {
  const board = boardPolicyBlock(profile.name);
  const [sclk, miso, mosi, cs, gdo0, gdo2] = profile.cc;
  const [genericTx, genericRx] = profile.generic;
  assert(board.includes(`.console = ${profile.policyConsole}`),
         `${profile.name} console transport policy missing`);
  assert(board.includes(profile.policyActivity), `${profile.name} activity LED policy missing`);
  for (const field of [
    `.sclk = ${sclk}`, `.miso = ${miso}`, `.mosi = ${mosi}`,
    `.cs = ${cs}`, `.gdo0 = ${gdo0}`, `.gdo2 = ${gdo2}`,
    `.generic_tx = ${genericTx}`, `.generic_rx = ${genericRx}`,
  ]) assert(board.includes(field), `${profile.name} policy missing ${field}`);

  const defaults = read("boards", profile.name, "sdkconfig.defaults");
  for (const [key, value] of [
    ["SCLK", sclk], ["MISO", miso], ["MOSI", mosi],
    ["CS", cs], ["GDO0", gdo0], ["GDO2", gdo2],
  ]) assert(defaults.includes(`CONFIG_CC1101_${key === "SCLK" || key === "MISO" || key === "MOSI" || key === "CS" ? `SPI_${key}_GPIO` : `${key}_GPIO`}=${value}`),
            `${profile.name} defaults missing CC1101 ${key}`);

  assert(readme.includes(profile.readmeProfile), `${profile.name} README profile row missing`);
  assert(readme.includes(profile.readmePins), `${profile.name} README GPIO row missing`);
  assert(readme.includes(`build/${profile.name}/esp32-cc1101.bin`),
         `${profile.name} README image path missing`);
  assert(readme.includes(`tools/push-ota.sh <effective-hostname>.local build/${profile.name}/esp32-cc1101.bin`),
         `${profile.name} README OTA command missing`);
  assert(tooling.includes(`build/${profile.name}/esp32-cc1101.bin`),
         `${profile.name} tooling image path missing`);
  assert(tooling.includes(profile.toolingFacts), `${profile.name} tooling board facts missing`);
  assert(buildScript.includes(`${profile.name})`), `${profile.name} build profile missing`);
  assert(otaScript.includes(`image_profile="${profile.name}"`),
         `${profile.name} OTA image profile missing`);
  assert(otaScript.includes(`ota_slot_size=$((` + profile.slot + `))`),
         `${profile.name} OTA slot validation missing`);

  const partitions = compact(read("boards", profile.name, "partitions.csv")).replaceAll(" ", "");
  assert(partitions.includes(`ota_0,app,ota_0,0x020000,${profile.slot},`),
         `${profile.name} first OTA slot mismatch`);
  assert(partitions.includes(`ota_1,app,ota_1,${profile.secondOffset},${profile.slot},`),
         `${profile.name} second OTA slot mismatch`);
}

const portExample = read("tools", "board-ports.example.conf");
for (const key of [
  "CLASSIC_APPROVED_PORT", "N16R8_APPROVED_PORT", "SUPERMINI_FH4R2_APPROVED_PORT",
]) {
  assert(tooling.includes(key), `tooling guide port configuration key missing: ${key}`);
  assert(buildScript.includes(key), `build wrapper port configuration key missing: ${key}`);
  assert(portExample.split("\n").includes(`${key}=`), `example must disable access by default: ${key}`);
}
assert(read(".gitignore").includes("/.local/"), "private configuration directory must be ignored");
assert(tooling.includes(".local/board-ports.conf") && readme.includes(".local/board-ports.conf"),
       "private board configuration workflow must be documented");
assert(!buildScript.includes("/dev/serial/by-id/usb-"), "build wrapper must not embed device identifiers");

for (const requirement of [
  "### First wired installation or migration",
  "Never run `erase-flash`",
  "0x9000",
  "0x6000",
  "embedded `RFBD` profile descriptor",
  "service mode `web` or `both`",
  "MQTT-only mode has no HTTP or OTA server",
  "**Web UI > System > Firmware update**",
  "intentionally unauthenticated",
  "Blocked until a persistent XIAO by-id path is supplied and explicitly approved",
]) assert(readme.includes(requirement), `README installation contract missing: ${requirement}`);

assert(tooling.includes("All four profiles are accepted by `tools/push-ota.sh`, including XIAO"),
       "tooling guide must distinguish XIAO network OTA from local wired access");
assert(tooling.includes("XIAO profile has no approved local wired path"),
       "tooling guide XIAO wired-access boundary missing");
assert(!readme.includes("/dev/ttyUSB") && !readme.includes("/dev/ttyACM"),
       "README must not recommend unstable serial device names");
for (const contract of [
  "Generic TX/RX GPIOs",
  "must not be connected simultaneously",
  "power removed",
]) {
  assert(readme.includes(contract) || readme.includes("Generic pins may overlap"),
         `README GPIO configuration warning missing: ${contract}`);
  assert(tooling.includes(contract) || tooling.includes("Generic pins may overlap"),
         `tooling GPIO configuration warning missing: ${contract}`);
}

console.log("Documentation contracts passed");
