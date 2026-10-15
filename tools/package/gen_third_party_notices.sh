#!/usr/bin/env bash
# Writes THIRD-PARTY-NOTICES.txt for a package folder: every library that came from the MSYS2 tree (with its version, licence and home page, from the
# pacman database) plus the few components that did not. Run in an MSYS2 UCRT64 shell: gen_third_party_notices.sh <package folder> <output file>
set -e
PKG=${1:?package folder}
OUT=${2:?output file}
PREFIX=${MSYSTEM_PREFIX:-/ucrt64}
{
  echo "THIRD-PARTY COMPONENTS OF THIS PACKAGE"
  echo "======================================"
  echo
  echo "SDR-Tiouner is distributed under the GNU General Public License v3 or later (see LICENSE.txt). It contains, or uses, the components below,"
  echo "each under its own licence. Source code of every component is available from the home page given, and from the MSYS2 project"
  echo "(https://github.com/msys2/MINGW-packages) for the packages built by it."
  echo
  echo "Built into the program or shipped next to it"
  echo "--------------------------------------------"
  echo "OnAir (receiver, player, signal processing) ........ GPL-3.0-or-later ... https://github.com/adswill/OnAir"
  echo "MediaMTX (WebRTC server, mediamtx.exe) .............. MIT ................ https://github.com/bluenviron/mediamtx   (MediaMTX-LICENSE.txt)"
  echo "libairspy 1.0.10 (libairspy.dll, patched close order) BSD-3-Clause ....... https://github.com/airspy/airspyone_host (Airspy-LICENSE.txt)"
  echo "LimeSuite (LimeSuite.dll, only when present) Apache-2.0 ................ https://github.com/myriadrf/LimeSuite (LimeSuite-LICENSE.txt)"
  echo "libiio (libiio.dll, PlutoSDR) LGPL-2.1-or-later ............................ https://github.com/analogdevicesinc/libiio (licenses/libiio-LGPL-2.1.txt)"
  echo "librtlsdr (librtlsdr.dll, RTL-SDR) GPL-2.0-or-later ........................... https://github.com/osmocom/rtl-sdr (licenses/librtlsdr-GPL-2.0.txt)"
  echo "libbladeRF (libbladeRF.dll, only when present) LGPL-2.1 ....................... https://github.com/Nuand/bladeRF (licenses/libbladeRF-LGPL-2.1.txt)"
  echo "libairspyhf (libairspyhf.dll, only when present) BSD-3-Clause ................. https://github.com/airspy/airspyhf (licenses/libairspyhf-BSD.txt)"
  echo "Bootstrap 5.3.3 (CSS of the web page) ............... MIT ................ https://getbootstrap.com               (Bootstrap-LICENSE.txt)"
  echo
  echo "Libraries from the MSYS2 UCRT64 distribution (DLLs in this folder)"
  echo "------------------------------------------------------------------"
  pkgs=$(for f in "$PKG"/*.dll "$PKG"/*.exe "$PKG"/lib/SoapySDR/modules*/*.dll; do
           [ -f "$f" ] || continue
           b=$(basename "$f")
           [ -f "$PREFIX/bin/$b" ] && pacman -Qoq "$PREFIX/bin/$b" 2>/dev/null
           [ -f "$PREFIX/lib/SoapySDR/modules0.8/$b" ] && pacman -Qoq "$PREFIX/lib/SoapySDR/modules0.8/$b" 2>/dev/null
         done | sort -u)
  for p in $pkgs; do
    pacman -Qi "$p" 2>/dev/null | awk -v name="$p" '
      function val(s) { sub(/^[^:]*: */, "", s); return s }
      /^Version/  { v = val($0) }
      /^URL/      { u = val($0) }
      /^Licenses/ { l = val($0); gsub(/spdx:/, "", l) }
      END { sub(/^mingw-w64-ucrt-x86_64-/, "", name); printf "%-26s %-14s %-30s %s\n", name, v, l, u }'
  done
  echo
  echo "Notes"
  echo "-----"
  echo "ffmpeg.exe is built with --enable-gpl --enable-version3 (x264, x265 and others): it is under the GNU GPL v3. Its complete source and build"
  echo "recipe: the MSYS2 package mingw-w64-ucrt-x86_64-ffmpeg (https://github.com/msys2/MINGW-packages) and https://ffmpeg.org/download.html."
  echo "The SDRplay API (sdrplay_api.dll) is NOT part of this package: install it from https://www.sdrplay.com/api/ (its own licence applies)."
} > "$OUT"
echo "$OUT: $(grep -c '' "$OUT") lines, $(echo "$pkgs" | wc -w) packages"
