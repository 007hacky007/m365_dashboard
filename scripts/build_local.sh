#!/usr/bin/env bash
set -euo pipefail

# Local build script for m365_dashboard using Arduino CLI (macOS friendly)
# - Installs arduino-cli via Homebrew if missing
# - Installs cores: arduino:avr and esp32:esp32 (with additional URL)
# - Installs libraries from repo ZIPs
# - Compiles AVR and ESP32 variants (incl. optional SIM_MODE)
#
# Usage:
#   scripts/build_local.sh              # build default targets
#   SIM=1 scripts/build_local.sh        # also build SIM_MODE variants
#   TARGETS="ProMini-16MHz ESP32-Dev" scripts/build_local.sh  # select targets
#   CLEAN=1 scripts/build_local.sh      # clean build-local/ before building

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
BUILD_DIR="${ROOT}/build-local"
SKETCH_DIR="${ROOT}/M365"
SIM=1

ADDITIONAL_URL="https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json"

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || return 1
}

step() { echo "[+] $*"; }
warn() { echo "[!] $*"; }
die()  { echo "[x] $*"; exit 1; }

# 1) Ensure Arduino CLI
if ! need_cmd arduino-cli; then
  step "arduino-cli not found; installing via Homebrew..."
  if ! need_cmd brew; then die "Homebrew not found. Install from https://brew.sh/"; fi
  brew install arduino-cli || die "Failed to install arduino-cli"
fi

# 2) Optional clean
if [[ "${CLEAN:-0}" == "1" ]]; then
  step "Cleaning ${BUILD_DIR}"
  rm -rf "${BUILD_DIR}"
fi
mkdir -p "${BUILD_DIR}"

# 3) Init/config Arduino CLI
step "Initializing Arduino CLI config"
arduino-cli config init || true
arduino-cli config set library.enable_unsafe_install true

# 4) Update index and install cores
step "Updating core index (incl. ESP32)"
arduino-cli core update-index --additional-urls "${ADDITIONAL_URL}"

step "Installing cores (arduino:avr, esp32:esp32)"
arduino-cli core install arduino:avr
arduino-cli core install esp32:esp32 --additional-urls "${ADDITIONAL_URL}"

# 5) Install required libraries from repo ZIPs
step "Installing libraries from ZIPs"
arduino-cli lib install --zip-path "${ROOT}/libraries/SSD1306Ascii.zip"
arduino-cli lib install --zip-path "${ROOT}/libraries/WatchDog-1.2.0.zip"

# 6) Target matrix (matches CI)
declare -a NAMES
declare -a FQBNS
declare -a FLAGS

add_target() {
  NAMES+=("$1"); FQBNS+=("$2"); FLAGS+=("$3");
}

add_target "ProMini-16MHz"     "arduino:avr:pro:cpu=16MHzatmega328" ""
add_target "ProMini-8MHz"       "arduino:avr:pro:cpu=8MHzatmega328"  ""
add_target "ESP32-Dev"          "esp32:esp32:esp32"                   ""

if [[ "${SIM:-0}" == "1" ]]; then
  add_target "ProMini-16MHz-SIM" "arduino:avr:pro:cpu=16MHzatmega328" "--build-property compiler.cpp.extra_flags=\"-DSIM_MODE\" --build-property compiler.c.extra_flags=\"-DSIM_MODE\""
  add_target "ProMini-8MHz-SIM"   "arduino:avr:pro:cpu=8MHzatmega328"  "--build-property compiler.cpp.extra_flags=\"-DSIM_MODE\" --build-property compiler.c.extra_flags=\"-DSIM_MODE\""
  add_target "ESP32-Dev-SIM"      "esp32:esp32:esp32"                  "--build-property compiler.cpp.extra_flags=\"-DSIM_MODE\" --build-property compiler.c.extra_flags=\"-DSIM_MODE\""
fi

# Filter targets via TARGETS env if provided
if [[ -n "${TARGETS:-}" ]]; then
  step "Filtering targets to: ${TARGETS}"
  mapfile -t WANT < <(echo "${TARGETS}" | tr ' ' '\n')
  tmpN=(); tmpF=(); tmpL=()
  for i in "${!NAMES[@]}"; do
    for w in "${WANT[@]}"; do
      if [[ "${NAMES[$i]}" == "$w" ]]; then
    tmpN+=("${NAMES[$i]}"); tmpF+=("${FQBNS[$i]}"); tmpL+=("${FLAGS[$i]}")
      fi
    done
  done
  NAMES=("${tmpN[@]}"); FQBNS=("${tmpF[@]}"); FLAGS=("${tmpL[@]}")
fi

# 7) Compile
for i in "${!NAMES[@]}"; do
  name="${NAMES[$i]}"; fqbn="${FQBNS[$i]}"; addf="${FLAGS[$i]}"
  outdir="${BUILD_DIR}/${name}"
  step "Compiling ${name} -> ${outdir}"
  mkdir -p "${outdir}"
  # shellcheck disable=SC2086
  arduino-cli compile \
    --fqbn "${fqbn}" \
    --export-binaries \
    --output-dir "${outdir}" \
    ${addf} \
    "${SKETCH_DIR}"
done

step "Build complete. Artifacts:"
find "${BUILD_DIR}" \( -name '*.hex' -o -name '*.bin' -o -name '*.elf' \) -print
