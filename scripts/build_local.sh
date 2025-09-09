#!/usr/bin/env bash
set -euo pipefail

# Simplified build script: ESP-IDF only.
# Removed all Arduino/arduino-cli logic per request.
# Usage:
#   scripts/build_local.sh          # build (esp32c3 target)
#   CLEAN=1 scripts/build_local.sh  # clean and build
#   IDF_TARGET=esp32s3 scripts/build_local.sh  # override target (default esp32c3)

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/.." && pwd)"
BUILD_DIR="${ROOT}/build-local"
IDF_PROJ_DIR="${ROOT}/esp-idf"
IDF_TARGET="${IDF_TARGET:-esp32c3}"

step() { echo "[+] $*"; }
die()  { echo "[x] $*"; exit 1; }

need_cmd() { command -v "$1" >/dev/null 2>&1; }

if ! need_cmd idf.py; then
  # Attempt auto-source of common ESP-IDF locations
  IDF_CANDIDATES=(
    "$HOME/esp/esp-idf/export.sh"
    "/opt/esp/idf/export.sh"
    "/usr/local/esp/esp-idf/export.sh"
  )
  for cand in "${IDF_CANDIDATES[@]}"; do
    if [[ -f "$cand" ]]; then
      step "Sourcing ESP-IDF environment: $cand"
      # shellcheck disable=SC1090
      source "$cand" || die "Failed sourcing $cand"
      break
    fi
  done
fi

if ! need_cmd idf.py; then
  die "idf.py not found after attempting auto-source. Please 'source <esp-idf>/export.sh' in your shell."
fi

if [[ "${CLEAN:-0}" == "1" ]]; then
  step "Cleaning build directories"
  rm -rf "${IDF_PROJ_DIR}/build" "${BUILD_DIR}/ESP32-C3-IDF"
fi

mkdir -p "${BUILD_DIR}"

pushd "${IDF_PROJ_DIR}" >/dev/null
step "Setting target ${IDF_TARGET}"
idf.py set-target "${IDF_TARGET}" >/dev/null
step "Building (idf.py build)"
idf.py build
popd >/dev/null

OUT_SUB="${BUILD_DIR}/ESP32-C3-IDF"
mkdir -p "${OUT_SUB}"
cp -f ${IDF_PROJ_DIR}/build/*.bin "${OUT_SUB}" 2>/dev/null || true
cp -f ${IDF_PROJ_DIR}/build/*.elf "${OUT_SUB}" 2>/dev/null || true
cp -f ${IDF_PROJ_DIR}/build/*.map "${OUT_SUB}" 2>/dev/null || true

step "ESP-IDF build complete. Artifacts in ${OUT_SUB}"
echo "Flash with: (from esp-idf dir) idf.py -p PORT flash"
