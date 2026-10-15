cmake_minimum_required(VERSION 3.20)
if(APPLE)
  project(SDR-Tiouner VERSION 0.1.0 LANGUAGES C CXX OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 20)
else()
  project(SDR-Tiouner VERSION 0.1.0 LANGUAGES C CXX)
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=onair)   # no build-machine paths inside the programs
  set(CMAKE_CXX_FLAGS_RELEASE "-O3")
  set(CMAKE_OBJCXX_FLAGS_RELEASE "-O3")
endif()

option(DECT2_BUILD_TESTS "Build the tests, tools and benchmarks" ON)
option(DECT2_PORTABLE "Use the portable DSP kernels instead of Accelerate (macOS)" OFF)
option(DECT2_WITH_SOAPY "Support generic radios (Airspy, SDRplay, RTL-SDR, PlutoSDR, LimeSDR, ...) through SoapySDR when it is installed" ON)
option(DECT2_NO_SIMD "Disable the SIMD intrinsics and use the plain C++ code" OFF)
option(DECT2_WIN_AVX2 "Windows and macOS on x86-64: build the receiver for AVX2 (all CPUs since 2013); these systems have no runtime dispatch like Linux" ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(HACKRF REQUIRED libhackrf)   # plain variables: some package managers list include paths that do not exist
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)   # the own HackRF driver
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswscale libswresample)
find_package(OpenSSL REQUIRED)
if(DECT2_WITH_SOAPY)
  pkg_check_modules(SOAPY SoapySDR)
endif()

# QO-100 DATV and shared signal-processing/playback utilities.
# dvbt.cpp/dvbt_fec.cpp provide the Reed-Solomon and Viterbi code also used by DVB-S.
add_library(dect2core STATIC
  core/src/source.cpp
  core/src/spectrum.cpp
  core/src/engine.cpp
  core/src/ldpc.cpp
  core/src/t2_tables.cpp
  core/src/t2fec_tables.cpp
  core/src/t2fec.cpp
  core/src/resampler.cpp
  core/src/clock_correct.cpp
  core/src/bbunpack.cpp
  core/src/ts.cpp
  core/src/tsout.cpp
  core/src/gain.cpp
  core/src/channel.cpp
  core/src/direction.cpp
  core/src/dsp_compat.cpp
  core/src/bandwidth.cpp
  core/src/quality.cpp
  core/src/teletext.cpp
  core/src/dvbt.cpp
  core/src/dvbt_fec.cpp
  core/src/player.cpp
  core/src/audioout.cpp
  core/src/conceal.cpp
  core/src/nettuner.cpp
  core/src/hls.cpp
  core/src/hackrf_usb.cpp
  core/src/demo_ts.cpp
  core/src/crash_report.cpp
  core/src/source_native.cpp
  core/src/source_limesdr.cpp
  core/src/netradio.cpp
  core/src/source_airspyhf.cpp
  core/src/source_bladerf.cpp
  core/src/source_hermeslite2.cpp
  core/src/source_rtltcp.cpp
  core/src/source_spyserver.cpp
  core/src/source_usrp.cpp
  core/src/rate_choice.cpp
  core/src/text_charset.cpp
  core/src/device_claims.cpp
  core/src/source_rtlsdr.cpp
  core/src/source_airspy.cpp
  core/src/source_plutosdr.cpp
  core/src/source_sdrplay.cpp
  core/src/usb_diag.cpp
  core/src/inflate.cpp
  core/src/open_url.cpp
  core/src/png.cpp
  core/src/iq_record.cpp
  core/src/channel_find.cpp
  core/src/exact_resampler.cpp
  core/src/multi_receiver.cpp
  core/src/modes.cpp
)
file(GLOB DECT2_DVBS_SRC CONFIGURE_DEPENDS core/src/dvbs_*.cpp)
target_sources(dect2core PRIVATE ${DECT2_DVBS_SRC})
target_include_directories(dect2core PUBLIC core/include)
if(SOAPY_FOUND)
  message(STATUS "SoapySDR ${SOAPY_VERSION}: generic radios enabled")
  target_sources(dect2core PRIVATE core/src/source_soapy.cpp)
  target_compile_definitions(dect2core PUBLIC DECT2_HAVE_SOAPY)
  target_include_directories(dect2core PRIVATE ${SOAPY_INCLUDE_DIRS})
  target_link_directories(dect2core PUBLIC ${SOAPY_LIBRARY_DIRS})
  target_link_libraries(dect2core PUBLIC ${SOAPY_LIBRARIES})
else()
  message(STATUS "SoapySDR not found: only the HackRF is supported (install libsoapysdr-dev / soapysdr to add other radios)")
endif()
# plain complex multiplication without the NaN/Inf fix-up calls (a large speed-up for std::complex<float>); the spelling depends on the compiler
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-fcx-limited-range DECT2_HAS_CX_LIMITED)
check_cxx_compiler_flag(-fcomplex-arithmetic=basic DECT2_HAS_COMPLEX_BASIC)
if(DECT2_HAS_CX_LIMITED)
  target_compile_options(dect2core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fcx-limited-range>)
elseif(DECT2_HAS_COMPLEX_BASIC)
  target_compile_options(dect2core PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fcomplex-arithmetic=basic>)
endif()
if(DECT2_PORTABLE)
  target_compile_definitions(dect2core PUBLIC DECT2_PORTABLE)
endif()
if(DECT2_NO_SIMD)
  target_compile_definitions(dect2core PUBLIC DECT2_NO_SIMD)
endif()
target_compile_definitions(dect2core PUBLIC ONAIR_VERSION="${PROJECT_VERSION}")
target_include_directories(dect2core PUBLIC ${HACKRF_INCLUDE_DIRS})
target_link_directories(dect2core PUBLIC ${HACKRF_LIBRARY_DIRS})
target_include_directories(dect2core PRIVATE ${LIBUSB_INCLUDE_DIRS})
target_link_directories(dect2core PUBLIC ${LIBUSB_LIBRARY_DIRS})
target_link_libraries(dect2core PUBLIC ${HACKRF_LIBRARIES} ${LIBUSB_LIBRARIES} PkgConfig::FFMPEG)
if(APPLE)
  # Metal LDPC decoder and CoreAudio output
  target_sources(dect2core PRIVATE core/src/gpu_ldpc.mm core/src/gpu_viterbi.mm core/src/audioout_coreaudio.mm core/src/conceal_apple.mm)
  set_source_files_properties(core/src/gpu_ldpc.mm core/src/gpu_viterbi.mm core/src/conceal_apple.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  if(DECT2_WIN_AVX2 AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64" OR CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64"))
    # Intel Mac: no runtime dispatch like on Linux, so the whole receiver is built for AVX2 (the program checks for it at start and says so if missing)
    target_compile_options(dect2core PRIVATE -mavx2 -mfma)
    target_compile_definitions(dect2core PUBLIC DECT2_REQUIRES_AVX2)
  endif()
  target_link_libraries(dect2core PUBLIC "-framework Accelerate" "-framework AudioToolbox" "-framework CoreAudio" "-framework CoreFoundation" "-framework Metal" "-framework Foundation" "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo" pthread iconv)
else()
  # no GPU decoder yet (stub), audio through miniaudio
  if(WIN32)
    target_sources(dect2core PRIVATE core/src/gpu_ldpc_d3d11.cpp core/src/gpu_viterbi_stub.cpp core/src/conceal_d3d11.cpp core/src/audioout_native.cpp core/src/audioout_miniaudio.cpp)   # Direct3D 11 compute
  else()
    target_sources(dect2core PRIVATE core/src/gpu_ldpc_stub.cpp core/src/gpu_viterbi_stub.cpp core/src/audioout_native.cpp core/src/audioout_miniaudio.cpp)
  endif()
  target_include_directories(dect2core PRIVATE third_party/miniaudio)
  find_package(Threads REQUIRED)
  target_link_libraries(dect2core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
  if(WIN32)
    # MinGW-w64 (MSYS2): Winsock for the network outputs, iphlpapi to list the computer's addresses
    target_compile_definitions(dect2core PUBLIC _USE_MATH_DEFINES WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
    target_link_libraries(dect2core PUBLIC ws2_32 iphlpapi shell32)
    target_link_libraries(dect2core PUBLIC dbghelp)   # minidump for the crash report
    if(DECT2_WIN_AVX2 AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64|x64" AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
      # no runtime dispatch on Windows: the whole receiver is built for AVX2 (the program checks for it at start and says so if missing)
      target_compile_options(dect2core PRIVATE -mavx2 -mfma)
      target_compile_definitions(dect2core PUBLIC DECT2_REQUIRES_AVX2)
    endif()
  endif()
  if(NOT WIN32)
    target_link_libraries(dect2core PUBLIC m)
  endif()
  find_package(Iconv QUIET)
  if(Iconv_FOUND AND NOT Iconv_IS_BUILT_IN)
    target_link_libraries(dect2core PUBLIC Iconv::Iconv)
  endif()
endif()

add_executable(onair_multi app/multi_main.cpp app/multi_web.cpp app/browser_stream.cpp app/batc_ws.cpp)
target_include_directories(onair_multi PRIVATE app core/src)
target_link_libraries(onair_multi PRIVATE dect2core OpenSSL::SSL OpenSSL::Crypto)
if(WIN32 AND EXISTS ${CMAKE_SOURCE_DIR}/packaging/windows/sdr_tiouner.rc)
  enable_language(RC)
  target_sources(onair_multi PRIVATE packaging/windows/sdr_tiouner.rc)   # the program's icon and version information
endif()

if(DECT2_BUILD_TESTS)
  enable_testing()
  file(GLOB DECT2_DATV_TESTS CONFIGURE_DEPENDS tests/test_dvbs_*.cpp)
  list(APPEND DECT2_DATV_TESTS
    tests/test_beacon_calibration.cpp tests/test_ts.cpp
    tests/test_tsout_udp.cpp tests/test_nettuner_auth.cpp tests/test_browser_picture.cpp tests/test_iq_correct.cpp tests/test_offset_tune.cpp
    tests/test_multi_receiver.cpp tests/test_auto_monitor.cpp
    tests/test_wideband_gain.cpp
    tests/test_iq_record.cpp tests/test_delivery_check.cpp
    tests/test_audio.cpp tests/test_conceal.cpp tests/test_crash_report.cpp tests/test_hls.cpp tests/test_png.cpp
    tests/test_aspect.cpp tests/test_adc_bits.cpp tests/test_bandwidth.cpp tests/test_direction.cpp tests/test_dsp.cpp
    tests/test_exact_resampler.cpp tests/test_teletext.cpp tests/test_fec.cpp tests/test_dvbt.cpp)
  foreach(src ${DECT2_DATV_TESTS})
    get_filename_component(target ${src} NAME_WE)
    string(REGEX REPLACE "^test_" "" test_name ${target})
    add_executable(${target} ${src})
    target_link_libraries(${target} PRIVATE dect2core)
    add_test(NAME ${test_name} COMMAND ${target})
    set_property(TEST ${test_name} APPEND PROPERTY ENVIRONMENT "DECT2_MUTE=1")
    # The tests decode in real time on the shared CPU/GPU: two at once drop frames, so ctest -j must not overlap them.
    set_property(TEST ${test_name} PROPERTY RESOURCE_LOCK dect2_realtime_decode)
  endforeach()
  if(APPLE OR WIN32)
    add_executable(test_gpu_ldpc tests/test_gpu_ldpc.cpp)
    target_link_libraries(test_gpu_ldpc PRIVATE dect2core)
  endif()

  if(UNIX)
    # the native radio drivers against fake vendor libraries
    add_library(native_fakes SHARED tests/native_fakes.cpp)
    set_target_properties(native_fakes PROPERTIES CXX_VISIBILITY_PRESET hidden)
    add_executable(test_native tests/test_native.cpp)
    target_link_libraries(test_native PRIVATE dect2core ${CMAKE_DL_LIBS})
    add_dependencies(test_native native_fakes)
    add_test(NAME native COMMAND test_native $<TARGET_FILE:native_fakes>)
    # the same fake libraries without the bias-tee functions (libraries from before they existed)
    add_library(native_fakes_nobias SHARED tests/native_fakes.cpp)
    set_target_properties(native_fakes_nobias PROPERTIES CXX_VISIBILITY_PRESET hidden)
    target_compile_definitions(native_fakes_nobias PRIVATE FAKE_NO_BIAS)
    add_dependencies(test_native native_fakes_nobias)
    add_test(NAME native_nobias COMMAND test_native $<TARGET_FILE:native_fakes_nobias> --nobias)
  endif()

  if(SOAPY_FOUND AND NOT WIN32)   # the test loads a plug-in module the POSIX way; Windows is not covered yet
    # a fake SoapySDR radio (plugin module) and a test that drives the generic-radio source through it
    set_target_properties(dect2core PROPERTIES POSITION_INDEPENDENT_CODE ON)
    add_library(SoapyOnAirTest MODULE tests/soapy_testdriver.cpp)
    target_include_directories(SoapyOnAirTest PRIVATE ${SOAPY_INCLUDE_DIRS})
    target_link_libraries(SoapyOnAirTest PRIVATE dect2core)
    add_executable(test_soapy tests/test_soapy.cpp)
    target_link_libraries(test_soapy PRIVATE dect2core)
    add_dependencies(test_soapy SoapyOnAirTest)
    add_test(NAME soapy COMMAND test_soapy $<TARGET_FILE_DIR:SoapyOnAirTest>)
  endif()
endif()
