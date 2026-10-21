#!/usr/bin/env bash
# Build the SDR-Tiouner Windows package (build-windows/SDR-Tiouner-windows-x64 and its zip).
# This package also carries the native Airspy R2 / Mini driver and its library (libairspy.dll, the close-order patched 1.0.10).
set -e
cd "$(dirname "$0")/../.."
PREFIX=${MSYSTEM_PREFIX:-/ucrt64}
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=Release -DDECT2_BUILD_TESTS=ON \
  -DCMAKE_C_COMPILER="$(cygpath -m "$PREFIX/bin/gcc.exe")" \
  -DCMAKE_CXX_COMPILER="$(cygpath -m "$PREFIX/bin/g++.exe")" -DCMAKE_SUPPRESS_REGENERATION=ON
cmake --build build-windows --target onair_multi -j8
OUT=build-windows/SDR-Tiouner-windows-x64
mkdir -p "$OUT"
# Everything the package carries besides the program comes from this project, so that it builds on its own:
#   runtime/windows-base      ffmpeg, MediaMTX and their DLLs (about 210 MB: not in git, see runtime/README.md)
#   third_party/radio-libs    the radio libraries (HackRF, Airspy, RTL-SDR, PlutoSDR, LimeSDR, bladeRF, Airspy HF+) and their licences
BASE=runtime/windows-base
LIBS=third_party/radio-libs
if [ ! -d "$BASE" ]; then echo "runtime/windows-base is missing: see runtime/README.md"; exit 1; fi
cp "$BASE"/*.dll "$OUT/"
cp "$BASE"/mediamtx.exe "$BASE"/ffmpeg.exe "$BASE"/MediaMTX-LICENSE.txt "$OUT/"
cp "$BASE"/LICENSE.txt "$OUT/"
if [ -d "$BASE/lib" ]; then cp -r "$BASE/lib" "$OUT/"; fi
# the radio libraries (loaded at run time from the program's folder; native_common.h libNames). The Airspy library is the close-order patched 1.0.10.
for lib in libairspy.dll libhackrf.dll libiio.dll librtlsdr.dll libbladeRF.dll libairspyhf.dll LimeSuite.dll; do
  if [ -f "$LIBS/$lib" ]; then cp "$LIBS/$lib" "$OUT/$lib"; elif [ ! -f "$OUT/$lib" ]; then echo "note: $lib is not in $LIBS: that radio needs it on the PC"; fi
done
mkdir -p "$OUT/licenses"
cp "$LIBS"/licenses/* "$OUT/licenses/"
cp "$LIBS/licenses/Airspy-LICENSE.txt" "$OUT/Airspy-LICENSE.txt"
cp "$LIBS/licenses/LimeSuite-LICENSE.txt" "$OUT/LimeSuite-LICENSE.txt"
strip -o "$OUT/SDR-Tiouner.exe" build-windows/onair_multi.exe
if [ ! -f "$OUT/multi-receiver.ini" ]; then
  for PREVIOUS in multi-receiver.ini; do
    [ -f "$PREVIOUS" ] && { cp "$PREVIOUS" "$OUT/"; break; }
  done
fi
# --- what a binary distribution of GPL software needs: the written offer of the source, the licences of the third-party parts, and the source
# itself to give on request. The contact in the offer comes from SDR_TIOUNER_CONTACT (an e-mail address or a web page).
sed -e "s/BUILD_DATE/$(date +%Y-%m-%d)/" -e "s|CONTACT_TO_BE_FILLED_IN|${SDR_TIOUNER_CONTACT:-CONTACT_TO_BE_FILLED_IN}|" SOURCE-OFFER.txt > "$OUT/SOURCE-OFFER.txt"
cp third_party/bootstrap/LICENSE "$OUT/Bootstrap-LICENSE.txt"
# (the list takes a couple of minutes: it asks the package database for every library; it is kept in build-windows, delete it to make it again)
[ -f build-windows/THIRD-PARTY-NOTICES.txt ] || bash tools/package/gen_third_party_notices.sh "$OUT" build-windows/THIRD-PARTY-NOTICES.txt
cp build-windows/THIRD-PARTY-NOTICES.txt "$OUT/THIRD-PARTY-NOTICES.txt"
# the licence texts of the MSYS2 libraries that travel in this folder (MIT, BSD ... licences require their notice to come with the binaries)
rm -rf "$OUT/licenses/msys2"; mkdir -p "$OUT/licenses/msys2"
for f in "$OUT"/*.dll "$OUT"/*.exe; do b=$(basename "$f"); [ -f "$PREFIX/bin/$b" ] && pacman -Qoq "$PREFIX/bin/$b" 2>/dev/null; done | sort -u | while read -r p; do
  n=${p#mingw-w64-ucrt-x86_64-}
  [ -d "$PREFIX/share/licenses/$n" ] && cp -r "$PREFIX/share/licenses/$n" "$OUT/licenses/msys2/"
done
rm -f build-windows/SDR-Tiouner-source.zip
# the source of exactly this version: only what is committed (git archive), so no local leftovers can end up in it
git archive --format=zip -o build-windows/SDR-Tiouner-source.zip HEAD
if grep -q CONTACT_TO_BE_FILLED_IN "$OUT/SOURCE-OFFER.txt"; then echo "WARNING: SOURCE-OFFER.txt has no contact yet: set SDR_TIOUNER_CONTACT before giving the package to anyone"; fi
cp MULTI_RECEIVER.md "$OUT/README.md"
cp AUTO_MONITOR.md "$OUT/AUTO_MONITOR.md"
cp LAN_VIEWING.md "$OUT/LAN_VIEWING.md"
cp tools/package/enable_lan_windows.ps1 "$OUT/Enable-LAN.ps1"
printf '@echo off\r\ncd /d "%%~dp0"\r\npowershell.exe -NoProfile -ExecutionPolicy Bypass -File "%%~dp0Enable-LAN.ps1"\r\npause\r\n' > "$OUT/Enable LAN access.cmd"
printf '@echo off\r\ncd /d "%%~dp0"\r\n"%%~dp0SDR-Tiouner.exe"\r\n' > "$OUT/Start SDR-Tiouner.cmd"
# The zip carries the clean settings of the source tree (no calibration, correction 0): the corrections in the working copy are the LNB and
# radios of this PC and would be wrong for anyone else. The working copy is put back afterwards.
cp "$OUT/multi-receiver.ini" "$OUT/multi-receiver.ini.dev"
cp multi-receiver.ini "$OUT/multi-receiver.ini"
sed -i '1s/^# .*/# SDR-Tiouner settings. Frequencies are satellite MHz./' "$OUT/multi-receiver.ini"
(cd build-windows && rm -f SDR-Tiouner-windows-x64.zip && zip -qr SDR-Tiouner-windows-x64.zip SDR-Tiouner-windows-x64 -x 'SDR-Tiouner-windows-x64/multi-receiver.ini.dev' -x 'SDR-Tiouner-windows-x64/multi-cache/*' -x 'SDR-Tiouner-windows-x64/recordings/*' -x '*.log' -x '*onair_multi.exe' -x '*SDR-Tiouner-preview.exe')
mv -f "$OUT/multi-receiver.ini.dev" "$OUT/multi-receiver.ini"
sed -i '1s/^# .*/# SDR-Tiouner settings. Frequencies are satellite MHz./' "$OUT/multi-receiver.ini"
echo "Ready: build-windows/SDR-Tiouner-windows-x64/SDR-Tiouner.exe   (zip: build-windows/SDR-Tiouner-windows-x64.zip)"
