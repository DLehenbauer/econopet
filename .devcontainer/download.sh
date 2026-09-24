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
readonly DOWNLOAD_TIMEOUT_SECONDS=30
readonly DOWNLOAD_RETRIES=2
mkdir -p "${DOWNLOAD_DIR}" "${MEDIA_DIR}/roms" "${MEDIA_DIR}/disks"

# Each manifest row has five pipe-separated fields:
#   primary URL | fallback URL | archive member | installed media path | installed-file MD5
# Use "-" when no fallback URL is available.
# The archive member selects a file from a zip.  Use "-" for raw or gzip input.
# A path ending in "/-" keeps the source or archive-member basename. Checksums
# cover the final extracted/copied media, not the downloaded file.
readonly MEDIA_LIST="$(cat <<'EOF'
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-c000.901439-01.bin|-|-|roms/-|b45778bdc95d67ccb475008718f4466b
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-d000.901439-02.bin|-|-|roms/-|e13b5675386beb93e8397df891113f5b
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-e000.901439-03.bin|-|-|roms/-|ef9bd0e62dfc47eb463fef20d0344826
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-f000.901439-04.bin|-|-|roms/-|2d44afcab9713ad4d11118a9e7429a81
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-c800.901439-05.bin|-|-|roms/-|d03ee896c37ad1bd86773655948e1174
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-d800.901439-06.bin|-|-|roms/-|673b61a8fe11cd5d7e4003f81a2d8453
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-f800.901439-07.bin|-|-|roms/-|9e1357c33c0d5a4a89b263f0c3c317ff
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/characters-1.901447-08.bin|-|-|roms/-|29a82eb54e73ebc5673c718c489b174b
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-2-c000.901439-09.bin|-|-|roms/-|9eaf58aa938083ad74117a81ac460e37
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-2-c000.901465-01.bin|-|-|roms/-|6b13eb6a7e4a2e15f3fff18461ce9c0d
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-2-d000.901465-02.bin|-|-|roms/-|8b7779f1afe3a6542bd17f6b80917d67
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-2-b.901474-01.bin|-|-|roms/-|7f87889ca7ee2537f0c1993d35d0fb18
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-2-n.901447-24.bin|-|-|roms/-|cb8e8404c0b28eda10469792dfd1dbc2
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/kernal-2.901465-03.bin|-|-|roms/-|51a38bfef8f9e72cb64bf7d874b4c8c6
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/characters-2.901447-10.bin|-|-|roms/-|9880432e633b15998d58884ff34c4e70
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-b000.901465-19.bin|-|-|roms/-|34d6650acd5dc4a4049f0c189ab5eeb0
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-c000.901465-20.bin|-|-|roms/-|398217f35fa50417c7e84883a93a349b
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-d000.901465-21.bin|-|-|roms/-|ab780e94772dca756a0678a17b5bc3a2
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-b.901474-02.bin|-|-|roms/-|a09d11163a708b8dea90f1c5df33dca0
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-n.901447-29.bin|-|-|roms/-|6fe27b43ec550a04d30b2e45f07d51fb
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-40-n-50Hz.901498-01.bin|-|-|roms/-|b76d756e7ac8752ae0035f3ce5f1383c
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-40-n-60Hz.901499-01.bin|-|-|roms/-|2e86403fc2ac30e7af05b9e8607bef98
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-80-b-50Hz.901474-04-3681.bin|-|-|roms/-|3e646194de7458b05a06159afbdd9427
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-80-b-60Hz.901474-03.bin|-|-|roms/-|da56995be008c5f7db1094e81e5060aa
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/kernal-4.901465-22.bin|-|-|roms/-|16ec21443ea5431ab63d511061054e6f
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-b000.901465-23.bin|-|-|roms/-|43b3a9f5e1c762af0b3bb6cc71aafb84
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-a000-bfff.970018-12.bin|-|-|roms/-|84cb402449c6107b7d2b636cb28f0042
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-c000-dfff.970019-12.bin|-|-|roms/-|0f9bd7123d99892ce80f1e7e438c2194
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-e000-ffff-970034-12.bin|-|-|roms/-|c03098d26dbb1a23737d3286f264bc97
https://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/characters.901640-01.bin|-|-|roms/-|dd30641d9e6a221edd725d1e529dcbdb
https://mikenaberezny.com/wp-content/uploads/2009/11/os9-systemdisk.d80|https://web.archive.org/web/20131106214937id_/http://mikenaberezny.com/wp-content/uploads/2009/11/os9-systemdisk.d80|-|disks/superpet/os9/-|a772ba7ad5d2f6a635a092f80f80d45c
https://www.zimmers.net/anonftp/pub/cbm/pet/SuperPET/Waterloo2-Language-1.d64.gz|-|-|disks/superpet/-|0c32478f4636b8387b62da3dde1f2acb
https://www.zimmers.net/anonftp/pub/cbm/pet/SuperPET/Waterloo2-Language-2.d64.gz|-|-|disks/superpet/-|9c9db97f8f56ae408b098abab6db7a5b
https://www.zimmers.net/anonftp/pub/cbm/pet/SuperPET/os9/testram9000.d80.gz|-|-|disks/superpet/-|0d062e32b06e0285fae2cc9800b97fc9
https://github.com/sjgray/cbm-edit-rom/raw/refs/heads/master/binaries/ColourPET/VICE/colourpet-c1-80-b-60-esc-wedge-reboot-backarrow-VICE%20(2017-03-03).bin|-|-|roms/colourpet-c1-80-b-60.bin|34fc9984ea9710d513a79ecec9a7bdb1
https://www.insanerocketry.com/personal/rom1diskmagic.zip|-|rom1diskmagic/rom1diskrom/rom1diskrom_v15.bin|roms/-|b13d678c248748a1440151b5f78fb385
EOF
)"

# Extract the lowercase checksum emitted by md5sum.
calc_md5() {
  md5sum "$1" | awk '{print $1}'
}

# Download one upstream source into the cache. Writing to a temporary name
# prevents an interrupted transfer from becoming a cache hit on the next run.
download_file() {
  local url="$1"
  local destination="$2"

  local partial="${destination}.part"
  if ! wget -q \
    --timeout="${DOWNLOAD_TIMEOUT_SECONDS}" \
    --tries="${DOWNLOAD_RETRIES}" \
    --user-agent="Mozilla/5.0" \
    -O "${partial}" \
    "${url}"; then
    echo "[ERR] Failed to download from ${url}" >&2
    rm -f "${partial}"
    return 1
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

# Normalize one cached download into a staged media file and verify its payload.
stage_media_file() {
  local download="$1"
  local member="$2"
  local partial="$3"
  local md5="$4"

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
  }; then
    echo "[ERR] Failed to extract $(basename "${download}")" >&2
    return 1
  fi

  verify_file "${partial}" "${md5}"
}

# Turn one manifest row into an installed media file.
install_media_file() {
  local url="$1"
  local fallback_url="$2"
  local member="$3"
  local media_path="$4"
  local md5="$5"
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
  local source_url

  mkdir -p "$(dirname "${destination}")"

  if [[ -f "${download}" ]]; then
    echo "[CACHED] $(basename "${download}")"
    if stage_media_file "${download}" "${member}" "${partial}" "${md5}"; then
      mv "${partial}" "${destination}"
      return
    fi
    rm -f "${partial}" "${download}"
  fi

  for source_url in "${url}" "${fallback_url}"; do
    [[ "${source_url}" == "-" ]] && continue
    if [[ "${source_url}" == "${fallback_url}" ]]; then
      echo "[FALLBACK] ${source_url}"
    else
      echo "[GET] ${source_url}"
    fi

    if ! download_file "${source_url}" "${download}"; then
      continue
    fi

    if ! stage_media_file "${download}" "${member}" "${partial}" "${md5}"; then
      rm -f "${partial}" "${download}"
      continue
    fi

    mv "${partial}" "${destination}"
    return
  done

  echo "[ERR] No valid download source for ${url}" >&2
  return 1
}

# Parse the manifest one row at a time and install each payload in order.
install_media_list() {
  local media_list="$1"
  local extra
  local fallback_url
  local md5
  local media_path
  local member
  local url

  while IFS='|' read -r url fallback_url member media_path md5 extra; do
    [[ -z "${url}" ]] && continue
    if [[ -z "${fallback_url}" || -z "${member}" || -z "${media_path}" ||
      -z "${md5}" || -n "${extra}" ]]; then
      echo "[ERR] Invalid media manifest row for ${url}" >&2
      return 1
    fi
    install_media_file "${url}" "${fallback_url}" "${member}" "${media_path}" "${md5}"
  done <<< "${media_list}"
}

# Populate all ROM and disk files before performing any collection-specific
# post-processing.
install_media_list "${MEDIA_LIST}"

# Waterloo2 was distributed as two D64s. Merge these into one D80, add the
# memory-test programs, then remove the temporary source images.
readonly WATERLOO_DISK_DIR="${MEDIA_DIR}/disks/superpet"
readonly WATERLOO_DISK_ONE="${WATERLOO_DISK_DIR}/Waterloo2-Language-1.d64"
readonly WATERLOO_DISK_TWO="${WATERLOO_DISK_DIR}/Waterloo2-Language-2.d64"
readonly WATERLOO_D80="${WATERLOO_DISK_DIR}/Waterloo2-Languages.d80"
readonly TESTRAM_D80="${WATERLOO_DISK_DIR}/testram9000.d80"
python3 "${SCRIPT_DIR}/merge-disks.py" "${WATERLOO_D80}.part" \
  --source "${WATERLOO_DISK_ONE}" '*' \
  --source "${WATERLOO_DISK_TWO}" '*' \
  --source "${TESTRAM_D80}" test.main test.banks test.os9
mv "${WATERLOO_D80}.part" "${WATERLOO_D80}"
rm -f "${WATERLOO_DISK_ONE}" "${WATERLOO_DISK_TWO}" "${TESTRAM_D80}"
