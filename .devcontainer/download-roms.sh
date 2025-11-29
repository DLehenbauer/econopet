#!/usr/bin/env bash
# Download PET ROMs and verify MD5 checksums.
# Usage: .devcontainer/download-roms.sh <destination_dir>
set -euo pipefail

usage() {
  echo "Usage: $0 <destination_dir>" >&2
  exit 2
}

# Require destination directory argument
if [[ $# -lt 1 ]]; then
  usage
fi

readonly OUT_DIR="$1"
mkdir -p "${OUT_DIR}"

# Create temporary directory that will auto-cleanup on exit
readonly TMP_DIR=$(mktemp -d)
trap "rm -rf '${TMP_DIR}'" EXIT

# List of Zimmers.net ROM files and their expected MD5 checksums
readonly ROM_LIST="$(cat <<'EOF'
rom-1-c000.901439-01.bin B45778BDC95D67CCB475008718F4466B
rom-1-d000.901439-02.bin E13B5675386BEB93E8397DF891113F5B
rom-1-e000.901439-03.bin EF9BD0E62DFC47EB463FEF20D0344826
rom-1-f000.901439-04.bin 2D44AFCAB9713AD4D11118A9E7429A81
rom-1-c800.901439-05.bin D03EE896C37AD1BD86773655948E1174
rom-1-d800.901439-06.bin 673B61A8FE11CD5D7E4003F81A2D8453
rom-1-f800.901439-07.bin 9E1357C33C0D5A4A89B263F0C3C317FF
characters-1.901447-08.bin 29A82EB54E73EBC5673C718C489B174B
rom-2-c000.901439-09.bin 9EAF58AA938083AD74117A81AC460E37
basic-2-c000.901465-01.bin 6B13EB6A7E4A2E15F3FFF18461CE9C0D
basic-2-d000.901465-02.bin 8B7779F1AFE3A6542BD17F6B80917D67
edit-2-b.901474-01.bin 7F87889CA7EE2537F0C1993D35D0FB18
edit-2-n.901447-24.bin CB8E8404C0B28EDA10469792DFD1DBC2
kernal-2.901465-03.bin 51A38BFEF8F9E72CB64BF7D874B4C8C6
characters-2.901447-10.bin 9880432E633B15998D58884FF34C4E70
basic-4-b000.901465-19.bin 34D6650ACD5DC4A4049F0C189AB5EEB0
basic-4-c000.901465-20.bin 398217F35FA50417C7E84883A93A349B
basic-4-d000.901465-21.bin AB780E94772DCA756A0678A17B5BC3A2
edit-4-b.901474-02.bin A09D11163A708B8DEA90F1C5DF33DCA0
edit-4-n.901447-29.bin 6FE27B43EC550A04D30B2E45F07D51FB
edit-4-40-n-50Hz.901498-01.bin B76D756E7AC8752AE0035F3CE5F1383C
edit-4-40-n-60Hz.901499-01.bin 2E86403FC2AC30E7AF05B9E8607BEF98
edit-4-80-b-50Hz.901474-04-3681.bin 3E646194DE7458B05A06159AFBDD9427
edit-4-80-b-60Hz.901474-03.bin DA56995BE008C5F7DB1094E81E5060AA
kernal-4.901465-22.bin 16EC21443EA5431AB63D511061054E6F
basic-4-b000.901465-23.bin 43B3A9F5E1C762AF0B3BB6CC71AAFB84
EOF
)"

calc_md5() {
  md5sum "$1" | awk '{print toupper($1)}'
}

# Verify MD5 checksum of a file, optionally downloading if missing or invalid
# Args: filepath, expected_md5, download_url (optional), download_path (optional)
# Returns: 0 if verified, 1 if downloaded, 2 download_url not provided and verification failed
ensure_file() {
  local filepath="$1"
  local want_md5="$2"
  local download_url="${3:-}"
  local download_path="${4:-${filepath}}"
  local filename="$(basename "${filepath}")"
  
  # Check if file exists and verify it
  if [[ -f "${filepath}" ]]; then
    local have_md5
    have_md5="$(calc_md5 "${filepath}")"
    
    if [[ "${have_md5}" == "${want_md5^^}" ]]; then
      echo "[OK] ${filename} (MD5: ${want_md5^^})"
      return 0
    else
      echo "[WARN] Checksum mismatch for cached ${filename}; re-downloading"
      rm -f "${filepath}"
    fi
  fi
  
  # Download if URL provided
  if [[ -n "${download_url}" ]]; then
    echo "[GET] ${download_url}"
    if ! wget -q -O "${download_path}" "${download_url}"; then
      echo "[ERR] Failed to download from ${download_url}"
      rm -f "${download_path}"
      exit 1
    fi
    return 1
  else
    echo "[ERR] MD5 mismatch for ${filename}" >&2
    echo "  expected: ${want_md5^^}" >&2
    echo "  actual:   ${have_md5}" >&2
    rm -f "${filepath}"
    return 2
  fi
}

download_roms() {
  local base_url="http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/"

  # Download and verify ROMs from list
  while read -r fname md5; do
    [[ -z "${fname:-}" ]] && continue
    
    local url="${base_url}${fname}"
    local dest="${OUT_DIR}/${fname}"
    
    # Try to download if needed (returns 1 if downloaded)
    if ! ensure_file "${dest}" "${md5}" "${url}"; then
      # Verify the downloaded file
      if ! ensure_file "${dest}" "${md5}"; then
        exit 1
      fi
    fi
  done <<< "${ROM_LIST}"
}

download_rom1diskmagic() {
  local url="https://www.insanerocketry.com/personal/rom1diskmagic.zip"
  local rom_file="rom1diskrom_v15.bin"
  local md5="B13D678C248748A1440151B5F78FB385"
  local dest="${OUT_DIR}/${rom_file}"
  local zip_path="${TMP_DIR}/$(basename "${url}")"

  # Check if file exists and is valid, otherwise download zip
  if ! ensure_file "${dest}" "${md5}" "${url}" "${zip_path}"; then
    # Extract the ROM file from the downloaded zip
    unzip -q "${zip_path}" -d "${TMP_DIR}"
    mv "${TMP_DIR}/rom1diskmagic/rom1diskrom/${rom_file}" "${dest}"
    
    # Verify the extracted file
    if ! ensure_file "${dest}" "${md5}"; then
      exit 1
    fi
  fi
}

download_colourpet_rom() {
  local url="https://github.com/sjgray/cbm-edit-rom/raw/refs/heads/master/binaries/ColourPET/VICE/colourpet-c1-80-b-60-esc-wedge-reboot-backarrow-VICE%20(2017-03-03).bin"
  local rom_file="colourpet-c1-80-b-60.bin"
  local md5="34FC9984EA9710D513A79ECEC9A7BDB1"
  local dest="${OUT_DIR}/${rom_file}"

  ensure_file "${dest}" "${md5}" "${url}" || {
    # Verify the downloaded file
    if ! ensure_file "${dest}" "${md5}"; then
      exit 1
    fi
  }
}

download_roms
download_rom1diskmagic
download_colourpet_rom
