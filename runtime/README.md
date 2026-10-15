# runtime/

`windows-base/` is the part of the Windows package that is not our code: ffmpeg and MediaMTX with the DLLs they need (about 210 MB,
the MSYS2 UCRT64 builds). It is not kept in git (too big). To make it again:

1. In an MSYS2 UCRT64 shell: `pacman -S mingw-w64-ucrt-x86_64-ffmpeg` and copy `ffmpeg.exe` and every DLL it needs
   (`ldd /ucrt64/bin/ffmpeg.exe | grep ucrt64`) into `runtime/windows-base/`.
2. Put `mediamtx.exe` (MIT licence, https://github.com/bluenviron/mediamtx) next to them, with its licence as `MediaMTX-LICENSE.txt`.
3. Add `LICENSE.txt` (the GPL text) and `libhackrf.dll`, `libSoapySDR.dll`, `libusb-1.0.dll`, `libwinpthread-1.dll` from the same MSYS2 tree.
