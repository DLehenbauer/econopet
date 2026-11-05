#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Reconstruct drive ROM assembly from pinned cbmsrc sources and original binaries.
# Clone the archived source, download hash-verified ROMs, and invoke verify.py to
# assemble source adaptations and disassembled controller/embedded payloads.
# Compare generated ROM bytes against the originals. Source checks exclude
# declared checksum placeholders and report uncovered regions. Reconstructed
# binary payloads must match every selected byte.
# Retain generated source, compiled images, reports, and logs under
# .tmp/ai/drive-roms/out. Source checkouts and downloads stay in separate inputs.
# Usage: tools/drive-roms/verify.sh [--help]

set -euo pipefail

# Explain the network operations and fixed private workspace.
usage() {
  echo "Usage: $0 [--help]"
  echo "Clone pinned cbmsrc and download approved drive ROMs under .tmp/ai/."
  echo "Reconstruct and verify into .tmp/ai/drive-roms/out (must not exist)."
}

if [[ $# -eq 1 && "${1:-}" == "--help" ]]; then
  usage
  exit 0
fi
if [[ $# -ne 0 ]]; then
  usage >&2
  exit 2
fi

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd -- "${SCRIPT_DIR}/../.." && pwd)"
readonly SCRIPT_DIR ROOT_DIR
readonly MANIFEST="${SCRIPT_DIR}/drive-emulation-roms.json"
readonly SOURCE_DIR="${ROOT_DIR}/.tmp/ai/cbmsrc"
readonly DOWNLOAD_DIR="${ROOT_DIR}/.tmp/ai/drive-roms/download"
readonly OUTPUT_DIR="${ROOT_DIR}/.tmp/ai/drive-roms/out"
readonly DOWNLOAD_TIMEOUT_SECONDS=120
readonly DOWNLOAD_RETRIES=2

# Fail before network or cache changes when prerequisites or the output are unsuitable.
for tool in git curl python3 acme da65 ca65 ld65 mktemp; do
  if ! command -v "${tool}" >/dev/null; then
    echo "Missing required tool: ${tool}" >&2
    exit 2
  fi
done
if [[ -e "${OUTPUT_DIR}" || -L "${OUTPUT_DIR}" ]]; then
  echo "Output already exists, preserve or move it before rerunning: ${OUTPUT_DIR}" >&2
  exit 2
fi

SOURCE_SPEC="$(python3 - "${MANIFEST}" <<'PY'
import json
from pathlib import Path
import sys

source = json.loads(Path(sys.argv[1]).read_text(encoding="ascii"))["source"]
print(source["repository"], source["commit"], sep="\t")
PY
)"
IFS=$'\t' read -r SOURCE_URL SOURCE_COMMIT <<< "${SOURCE_SPEC}"
readonly SOURCE_URL SOURCE_COMMIT

# Never reset or overwrite an existing source checkout.
if [[ -e "${SOURCE_DIR}" || -L "${SOURCE_DIR}" ]]; then
  if [[ ! -e "${SOURCE_DIR}/.git" ]]; then
    echo "Existing source directory is not a Git checkout: ${SOURCE_DIR}" >&2
    exit 2
  fi
  existing_origin="$(git -C "${SOURCE_DIR}" remote get-url origin)"
  existing_commit="$(git -C "${SOURCE_DIR}" rev-parse HEAD)"
  existing_status="$(git -C "${SOURCE_DIR}" status --porcelain --untracked-files=all)"
  if [[ "${existing_origin}" != "${SOURCE_URL}" ]] \
      || [[ "${existing_commit}" != "${SOURCE_COMMIT}" ]] \
      || [[ -n "${existing_status}" ]]; then
    echo "Existing cbmsrc must be clean and match the pinned origin/commit: ${SOURCE_DIR}" >&2
    exit 2
  fi
else
  mkdir -p -- "$(dirname -- "${SOURCE_DIR}")"
  GIT_TERMINAL_PROMPT=0 git clone --filter=blob:none --no-checkout "${SOURCE_URL}" "${SOURCE_DIR}"
  git -C "${SOURCE_DIR}" sparse-checkout set DOS_4040 DOS_8250 CONTROLLER_8250
  git -C "${SOURCE_DIR}" checkout --detach "${SOURCE_COMMIT}"
fi

# Validate cached and downloaded bytes before publishing a ROM cache entry.
verify_rom() {
  python3 - "$1" "$2" "$3" <<'PY'
import hashlib
from pathlib import Path
import sys

path, size, expected = Path(sys.argv[1]), int(sys.argv[2]), sys.argv[3]
data = path.read_bytes()
if len(data) != size or hashlib.sha256(data).hexdigest() != expected:
    raise SystemExit(f"ROM identity mismatch: {path.name}")
PY
}

# Remove only this invocation's incomplete download, retaining all prior evidence.
PARTIAL=""
cleanup() {
  if [[ -n "${PARTIAL}" ]]; then
    rm -f -- "${PARTIAL}"
  fi
}
trap cleanup EXIT

ROM_ROWS="$(python3 - "${MANIFEST}" <<'PY'
import json
from pathlib import Path
import sys

manifest = json.loads(Path(sys.argv[1]).read_text(encoding="ascii"))
for rom in manifest["drive_roms"]:
    print(rom["part"], rom["size"], rom["sha256"], rom["url"], sep="\t")
PY
)"
mkdir -p -- "${DOWNLOAD_DIR}"
while IFS=$'\t' read -r part size sha256 url; do
  destination="${DOWNLOAD_DIR}/${part}.bin"
  if [[ ! -e "${destination}" ]]; then
    PARTIAL="$(mktemp "${destination}.part.XXXXXX")"
    curl --fail --location --silent --show-error \
      --retry "${DOWNLOAD_RETRIES}" --max-time "${DOWNLOAD_TIMEOUT_SECONDS}" \
      --output "${PARTIAL}" "${url}"
    verify_rom "${PARTIAL}" "${size}" "${sha256}"
    mv -- "${PARTIAL}" "${destination}"
    PARTIAL=""
  else
    verify_rom "${destination}" "${size}" "${sha256}"
  fi
  echo "Verified ${part}.bin"
done <<< "${ROM_ROWS}"

# Reconstruct both source mappings and all selected payloads, then compare bytes.
python3 "${SCRIPT_DIR}/verify.py" \
  --source-root "${SOURCE_DIR}" \
  --rom-dir "${DOWNLOAD_DIR}" \
  --manifest "${MANIFEST}" \
  --output-dir "${OUTPUT_DIR}"
