#!/usr/bin/env bash
# SPDX-License-Identifier: CC0-1.0
# https://github.com/dlehenbauer/econopet
#
# Download PET ROMs and SuperPET disks, then verify MD5 checksums.
#
# Upstream files are retained verbatim under downloads/. Installed media is
# copied or extracted under media/roms and media/disks.
#
# Usage:
#   .devcontainer/download.sh <install_dir>

set -euo pipefail

usage() {
  echo "Usage: $0 <install_dir>" >&2
  exit 2
}

# Require <install_dir> argument
if [[ $# -lt 1 ]]; then
  usage
fi

readonly INSTALL_DIR="$1"
readonly DOWNLOAD_DIR="${INSTALL_DIR}/downloads"
readonly MEDIA_DIR="${INSTALL_DIR}/media"
readonly SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
mkdir -p "${DOWNLOAD_DIR}" "${MEDIA_DIR}/roms" "${MEDIA_DIR}/disks"

# Each manifest row has four pipe-separated fields:
#   URL | archive member | installed media path | installed-file MD5
# The archive member selects a file from a zip.  Use "-" for raw or gzip input.
# A path ending in "/-" keeps the source or archive-member basename. Checksums
# cover the final extracted/copied media, not the downloaded file.
readonly MEDIA_LIST="$(cat <<'EOF'
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-c000.901439-01.bin|-|roms/-|b45778bdc95d67ccb475008718f4466b
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-d000.901439-02.bin|-|roms/-|e13b5675386beb93e8397df891113f5b
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-e000.901439-03.bin|-|roms/-|ef9bd0e62dfc47eb463fef20d0344826
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-f000.901439-04.bin|-|roms/-|2d44afcab9713ad4d11118a9e7429a81
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-c800.901439-05.bin|-|roms/-|d03ee896c37ad1bd86773655948e1174
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-d800.901439-06.bin|-|roms/-|673b61a8fe11cd5d7e4003f81a2d8453
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-f800.901439-07.bin|-|roms/-|9e1357c33c0d5a4a89b263f0c3c317ff
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/characters-1.901447-08.bin|-|roms/-|29a82eb54e73ebc5673c718c489b174b
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-2-c000.901439-09.bin|-|roms/-|9eaf58aa938083ad74117a81ac460e37
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-2-c000.901465-01.bin|-|roms/-|6b13eb6a7e4a2e15f3fff18461ce9c0d
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-2-d000.901465-02.bin|-|roms/-|8b7779f1afe3a6542bd17f6b80917d67
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-2-b.901474-01.bin|-|roms/-|7f87889ca7ee2537f0c1993d35d0fb18
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-2-n.901447-24.bin|-|roms/-|cb8e8404c0b28eda10469792dfd1dbc2
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/kernal-2.901465-03.bin|-|roms/-|51a38bfef8f9e72cb64bf7d874b4c8c6
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/characters-2.901447-10.bin|-|roms/-|9880432e633b15998d58884ff34c4e70
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-b000.901465-19.bin|-|roms/-|34d6650acd5dc4a4049f0c189ab5eeb0
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-c000.901465-20.bin|-|roms/-|398217f35fa50417c7e84883a93a349b
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-d000.901465-21.bin|-|roms/-|ab780e94772dca756a0678a17b5bc3a2
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-b.901474-02.bin|-|roms/-|a09d11163a708b8dea90f1c5df33dca0
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-n.901447-29.bin|-|roms/-|6fe27b43ec550a04d30b2e45f07d51fb
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-40-n-50Hz.901498-01.bin|-|roms/-|b76d756e7ac8752ae0035f3ce5f1383c
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-40-n-60Hz.901499-01.bin|-|roms/-|2e86403fc2ac30e7af05b9e8607bef98
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-80-b-50Hz.901474-04-3681.bin|-|roms/-|3e646194de7458b05a06159afbdd9427
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-80-b-60Hz.901474-03.bin|-|roms/-|da56995be008c5f7db1094e81e5060aa
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/kernal-4.901465-22.bin|-|roms/-|16ec21443ea5431ab63d511061054e6f
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-b000.901465-23.bin|-|roms/-|43b3a9f5e1c762af0b3bb6cc71aafb84
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-a000-bfff.970018-12.bin|-|roms/-|84cb402449c6107b7d2b636cb28f0042
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-c000-dfff.970019-12.bin|-|roms/-|0f9bd7123d99892ce80f1e7e438c2194
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-e000-ffff-970034-12.bin|-|roms/-|c03098d26dbb1a23737d3286f264bc97
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/characters.901640-01.bin|-|roms/-|dd30641d9e6a221edd725d1e529dcbdb
https://mikenaberezny.com/wp-content/uploads/2009/11/os9-systemdisk.d80|-|disks/superpet/os9/-|a772ba7ad5d2f6a635a092f80f80d45c
https://www.zimmers.net/anonftp/pub/cbm/pet/SuperPET/Waterloo2-Language-1.d64.gz|-|disks/superpet/-|0c32478f4636b8387b62da3dde1f2acb
https://www.zimmers.net/anonftp/pub/cbm/pet/SuperPET/Waterloo2-Language-2.d64.gz|-|disks/superpet/-|9c9db97f8f56ae408b098abab6db7a5b
https://github.com/sjgray/cbm-edit-rom/raw/refs/heads/master/binaries/ColourPET/VICE/colourpet-c1-80-b-60-esc-wedge-reboot-backarrow-VICE%20(2017-03-03).bin|-|roms/colourpet-c1-80-b-60.bin|34fc9984ea9710d513a79ecec9a7bdb1
https://www.insanerocketry.com/personal/rom1diskmagic.zip|rom1diskmagic/rom1diskrom/rom1diskrom_v15.bin|roms/-|b13d678c248748a1440151b5f78fb385
EOF
)"

# Extract the lowercase checksum emitted by md5sum.
calc_md5() {
  md5sum "$1" | awk '{print $1}'
}

# Download an upstream file once and retain it verbatim in the cache. Writing
# to a temporary name prevents an interrupted transfer from becoming a cache
# hit on the next run.
download_file() {
  local url="$1"
  local destination="$2"

  if [[ -f "${destination}" ]]; then
    echo "[CACHED] $(basename "${destination}")"
    return
  fi

  local partial="${destination}.part"
  echo "[GET] ${url}"
  if ! wget -q --user-agent="Mozilla/5.0" -O "${partial}" "${url}"; then
    echo "[ERR] Failed to download from ${url}" >&2
    rm -f "${partial}"
    exit 1
  fi
  mv "${partial}" "${destination}"
}

# Verify the installed payload matches the given MD5 after decompression/extraction.
verify_file() {
  local filepath="$1"
  local want_md5="$2"
  local have_md5
  have_md5="$(calc_md5 "${filepath}")"

  if [[ "${have_md5}" != "${want_md5}" ]]; then
    echo "[ERR] MD5 mismatch for $(basename "${filepath}")" >&2
    echo "  expected: ${want_md5}" >&2
    echo "  actual:   ${have_md5}" >&2
    return 1
  fi
  echo "[OK] $(basename "${filepath}") (MD5: ${want_md5})"
}

# Turn one manifest row into an installed media file.
install_media_file() {
  local url="$1"
  local member="$2"
  local media_path="$3"
  local md5="$4"
  local filename

  # Resolve a trailing "-" to the archive member name when present, otherwise
  # to the downloaded filename with a gzip suffix removed.
  if [[ "${media_path##*/}" == "-" ]]; then
    if [[ "${member}" == "-" ]]; then
      filename="${url##*/}"
      filename="${filename%.gz}"
    else
      filename="${member##*/}"
    fi
    media_path="${media_path%/*}/${filename}"
  fi
  local download="${DOWNLOAD_DIR}/${url##*/}"
  local destination="${MEDIA_DIR}/${media_path}"
  local partial="${destination}.part"
  local attempt

  mkdir -p "$(dirname "${destination}")"

  for attempt in 1 2; do
    download_file "${url}" "${download}"

    # Normalize zip, gzip, and raw sources into one staged destination file.
    if ! {
      case "${download}" in
        *.zip)
          unzip -p "${download}" "${member}" > "${partial}"
          ;;
        *.gz)
          gzip -dc "${download}" > "${partial}"
          ;;
        *)
          cp "${download}" "${partial}"
          ;;
      esac
    } || ! verify_file "${partial}" "${md5}"; then
      rm -f "${partial}" "${download}"
      if [[ "${attempt}" -eq 2 ]]; then
        return 1
      fi
      echo "[RETRY] Re-downloading $(basename "${download}")"
      continue
    fi

    mv "${partial}" "${destination}"
    return
  done
}

# Parse the manifest one row at a time and install each payload in order.
install_media_list() {
  local media_list="$1"

  while IFS='|' read -r url member media_path md5; do
    [[ -z "${url:-}" ]] && continue
    install_media_file "${url}" "${member}" "${media_path}" "${md5}"
  done <<< "${media_list}"
}

# Populate all ROM and disk files before performing any collection-specific
# post-processing.
install_media_list "${MEDIA_LIST}"

# Waterloo2 was distributed as two D64s. Merge these into one D80, then remove
# the temporary source images.
readonly WATERLOO_DISK_DIR="${MEDIA_DIR}/disks/superpet"
readonly WATERLOO_DISK_ONE="${WATERLOO_DISK_DIR}/Waterloo2-Language-1.d64"
readonly WATERLOO_DISK_TWO="${WATERLOO_DISK_DIR}/Waterloo2-Language-2.d64"
readonly WATERLOO_D80="${WATERLOO_DISK_DIR}/Waterloo2-Languages.d80"
python3 "${SCRIPT_DIR}/merge-d64-to-d80.py" "${WATERLOO_D80}.part" \
  "${WATERLOO_DISK_ONE}" "${WATERLOO_DISK_TWO}"
mv "${WATERLOO_D80}.part" "${WATERLOO_D80}"
rm -f "${WATERLOO_DISK_ONE}" "${WATERLOO_DISK_TWO}"
