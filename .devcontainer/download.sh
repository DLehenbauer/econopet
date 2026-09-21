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

# Require parent directory argument
if [[ $# -lt 1 ]]; then
  usage
fi

readonly INSTALL_DIR="$1"
readonly DOWNLOAD_DIR="${INSTALL_DIR}/downloads"
readonly MEDIA_DIR="${INSTALL_DIR}/media"
mkdir -p "${DOWNLOAD_DIR}" "${MEDIA_DIR}/roms" "${MEDIA_DIR}/disks"

# URL | archive member (- for raw or gzip) | installed media path (- to keep basename) | MD5
readonly MEDIA_LIST="$(cat <<'EOF'
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-c000.901439-01.bin|-|roms/-|B45778BDC95D67CCB475008718F4466B
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-d000.901439-02.bin|-|roms/-|E13B5675386BEB93E8397DF891113F5B
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-e000.901439-03.bin|-|roms/-|EF9BD0E62DFC47EB463FEF20D0344826
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-f000.901439-04.bin|-|roms/-|2D44AFCAB9713AD4D11118A9E7429A81
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-c800.901439-05.bin|-|roms/-|D03EE896C37AD1BD86773655948E1174
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-d800.901439-06.bin|-|roms/-|673B61A8FE11CD5D7E4003F81A2D8453
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-1-f800.901439-07.bin|-|roms/-|9E1357C33C0D5A4A89B263F0C3C317FF
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/characters-1.901447-08.bin|-|roms/-|29A82EB54E73EBC5673C718C489B174B
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/rom-2-c000.901439-09.bin|-|roms/-|9EAF58AA938083AD74117A81AC460E37
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-2-c000.901465-01.bin|-|roms/-|6B13EB6A7E4A2E15F3FFF18461CE9C0D
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-2-d000.901465-02.bin|-|roms/-|8B7779F1AFE3A6542BD17F6B80917D67
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-2-b.901474-01.bin|-|roms/-|7F87889CA7EE2537F0C1993D35D0FB18
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-2-n.901447-24.bin|-|roms/-|CB8E8404C0B28EDA10469792DFD1DBC2
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/kernal-2.901465-03.bin|-|roms/-|51A38BFEF8F9E72CB64BF7D874B4C8C6
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/characters-2.901447-10.bin|-|roms/-|9880432E633B15998D58884FF34C4E70
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-b000.901465-19.bin|-|roms/-|34D6650ACD5DC4A4049F0C189AB5EEB0
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-c000.901465-20.bin|-|roms/-|398217F35FA50417C7E84883A93A349B
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-d000.901465-21.bin|-|roms/-|AB780E94772DCA756A0678A17B5BC3A2
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-b.901474-02.bin|-|roms/-|A09D11163A708B8DEA90F1C5DF33DCA0
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-n.901447-29.bin|-|roms/-|6FE27B43EC550A04D30B2E45F07D51FB
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-40-n-50Hz.901498-01.bin|-|roms/-|B76D756E7AC8752AE0035F3CE5F1383C
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-40-n-60Hz.901499-01.bin|-|roms/-|2E86403FC2AC30E7AF05B9E8607BEF98
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-80-b-50Hz.901474-04-3681.bin|-|roms/-|3E646194DE7458B05A06159AFBDD9427
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/edit-4-80-b-60Hz.901474-03.bin|-|roms/-|DA56995BE008C5F7DB1094E81E5060AA
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/kernal-4.901465-22.bin|-|roms/-|16EC21443EA5431AB63D511061054E6F
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/basic-4-b000.901465-23.bin|-|roms/-|43B3A9F5E1C762AF0B3BB6CC71AAFB84
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-a000-bfff.970018-12.bin|-|roms/-|84CB402449C6107B7D2B636CB28F0042
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-c000-dfff.970019-12.bin|-|roms/-|0F9BD7123D99892CE80F1E7E438C2194
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/waterloo-e000-ffff-970034-12.bin|-|roms/-|C03098D26DBB1A23737D3286F264BC97
http://www.zimmers.net/anonftp/pub/cbm/firmware/computers/pet/SuperPET/characters.901640-01.bin|-|roms/-|DD30641D9E6A221EDD725D1E529DCBDB
https://mikenaberezny.com/wp-content/uploads/2009/11/os9-systemdisk.d80|-|disks/superpet/os9/-|A772BA7AD5D2F6A635A092F80F80D45C
https://www.zimmers.net/anonftp/pub/cbm/pet/SuperPET/Waterloo2-Language-1.d64.gz|-|disks/superpet/-|0C32478F4636B8387B62DA3DDE1F2ACB
https://www.zimmers.net/anonftp/pub/cbm/pet/SuperPET/Waterloo2-Language-2.d64.gz|-|disks/superpet/-|9C9DB97F8F56AE408B098ABAB6DB7A5B
https://github.com/sjgray/cbm-edit-rom/raw/refs/heads/master/binaries/ColourPET/VICE/colourpet-c1-80-b-60-esc-wedge-reboot-backarrow-VICE%20(2017-03-03).bin|-|roms/colourpet-c1-80-b-60.bin|34FC9984EA9710D513A79ECEC9A7BDB1
https://www.insanerocketry.com/personal/rom1diskmagic.zip|rom1diskmagic/rom1diskrom/rom1diskrom_v15.bin|roms/-|B13D678C248748A1440151B5F78FB385
EOF
)"

calc_md5() {
  md5sum "$1" | awk '{print toupper($1)}'
}

# Download an upstream file without transforming it.
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

verify_file() {
  local filepath="$1"
  local want_md5="$2"
  local have_md5
  have_md5="$(calc_md5 "${filepath}")"

  if [[ "${have_md5}" != "${want_md5^^}" ]]; then
    echo "[ERR] MD5 mismatch for $(basename "${filepath}")" >&2
    echo "  expected: ${want_md5^^}" >&2
    echo "  actual:   ${have_md5}" >&2
    return 1
  fi
  echo "[OK] $(basename "${filepath}") (MD5: ${want_md5^^})"
}

install_media_file() {
  local url="$1"
  local member="$2"
  local media_path="$3"
  local md5="$4"
  local filename
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

  mkdir -p "$(dirname "${destination}")"
  download_file "${url}" "${download}"

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

  verify_file "${partial}" "${md5}" || exit 1
  mv "${partial}" "${destination}"
}

install_media_list() {
  local media_list="$1"

  while IFS='|' read -r url member media_path md5; do
    [[ -z "${url:-}" ]] && continue
    install_media_file "${url}" "${member}" "${media_path}" "${md5}"
  done <<< "${media_list}"
}

install_media_list "${MEDIA_LIST}"
