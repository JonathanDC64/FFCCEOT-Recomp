# Third-party code built from pinned git submodules.

# ---- melonDS core (GPL-3.0-or-later) ----
# Built from our own source list rather than melonDS's CMake: we need the emulation core only (no frontend,
# JIT, OpenGL renderer or GDB stub), and the recompiled ARM9 replaces melonDS's ARM9 interpreter at run time.
set(MELONDS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/melonDS)
if(NOT EXISTS ${MELONDS_DIR}/src/NDS.cpp)
  message(FATAL_ERROR "third_party/melonDS is missing: run `git submodule update --init --recursive`")
endif()
set(MS ${MELONDS_DIR}/src)

set(melonDS_HOMEPAGE_URL "https://melonds.kuribo64.net")
set(melonDS_VERSION "1.1")
set(MELONDS_VERSION_SUFFIX "-ffcc")
configure_file(${MS}/version.h.in ${CMAKE_CURRENT_BINARY_DIR}/melonds_gen/version.h)

add_library(melonds_core STATIC
  ${MS}/ARCodeFile.cpp ${MS}/ARDatabaseDAT.cpp ${MS}/AREngine.cpp ${MS}/ARM.cpp ${MS}/ARMInterpreter.cpp
  ${MS}/ARMInterpreter_ALU.cpp ${MS}/ARMInterpreter_Branch.cpp ${MS}/ARMInterpreter_LoadStore.cpp ${MS}/CP15.cpp
  ${MS}/CRC32.cpp ${MS}/DMA.cpp ${MS}/DMA_Timings.cpp ${MS}/DSi.cpp ${MS}/DSi_AES.cpp ${MS}/DSi_Camera.cpp
  ${MS}/DSi_DSP.cpp ${MS}/DSi_I2C.cpp ${MS}/DSi_I2S.cpp ${MS}/DSi_NAND.cpp ${MS}/DSi_NDMA.cpp ${MS}/DSi_NWifi.cpp
  ${MS}/DSi_SD.cpp ${MS}/DSi_SPI_TSC.cpp ${MS}/FATIO.cpp ${MS}/FATStorage.cpp ${MS}/GBACart.cpp
  ${MS}/GBACartMotionPak.cpp ${MS}/GPU.cpp ${MS}/GPU_Soft.cpp ${MS}/GPU2D.cpp ${MS}/GPU2D_Soft.cpp ${MS}/GPU3D.cpp
  ${MS}/GPU3D_Soft.cpp ${MS}/GPU3D_Texcache.cpp ${MS}/Mic.cpp ${MS}/NDS.cpp ${MS}/NDSCart.cpp ${MS}/ROMList.cpp
  ${MS}/FreeBIOS.cpp ${MS}/RTC.cpp ${MS}/Savestate.cpp ${MS}/SPI.cpp ${MS}/SPI_Firmware.cpp ${MS}/SPU.cpp
  ${MS}/Utils.cpp ${MS}/Wifi.cpp ${MS}/WifiAP.cpp
  ${MS}/DSP_HLE/UcodeBase.cpp ${MS}/DSP_HLE/AACUcode.cpp ${MS}/DSP_HLE/G711Ucode.cpp ${MS}/DSP_HLE/GraphicsUcode.cpp
  ${MS}/NDSCart/CartCommon.cpp ${MS}/NDSCart/CartRetail.cpp ${MS}/NDSCart/CartRetailNAND.cpp
  ${MS}/NDSCart/CartRetailIR.cpp ${MS}/NDSCart/CartRetailBT.cpp ${MS}/NDSCart/CartSD.cpp
  ${MS}/NDSCart/CartHomebrew.cpp ${MS}/NDSCart/CartR4.cpp
  ${MS}/fatfs/ff.c ${MS}/fatfs/ffsystem.c ${MS}/fatfs/ffunicode.c
  ${MS}/sha1/sha1.c ${MS}/tiny-AES-c/aes.c ${MS}/xxhash/xxhash.c ${MS}/blip-buf/blip_buf.c)
set_target_properties(melonds_core PROPERTIES CXX_STANDARD 17)
target_include_directories(melonds_core SYSTEM PUBLIC ${MS} ${CMAKE_CURRENT_BINARY_DIR}/melonds_gen)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
  target_compile_definitions(melonds_core PUBLIC ARCHITECTURE_x86_64=1)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64|ARM64")
  target_compile_definitions(melonds_core PUBLIC ARCHITECTURE_ARM64=1)
endif()

# DSi DSP interpreter (unused by NDS games, but referenced by the DSi code that NDS.cpp links against).
add_library(melonds_teakra STATIC
  ${MS}/teakra/src/ahbm.cpp ${MS}/teakra/src/apbp.cpp ${MS}/teakra/src/btdmp.cpp ${MS}/teakra/src/disassembler.cpp
  ${MS}/teakra/src/dma.cpp ${MS}/teakra/src/timer.cpp ${MS}/teakra/src/memory_interface.cpp ${MS}/teakra/src/mmio.cpp
  ${MS}/teakra/src/parser.cpp ${MS}/teakra/src/processor.cpp ${MS}/teakra/src/teakra.cpp)
set_target_properties(melonds_teakra PROPERTIES CXX_STANDARD 17)
target_include_directories(melonds_teakra PUBLIC ${MS}/teakra/include PRIVATE ${MS}/teakra/src)
target_link_libraries(melonds_core PRIVATE melonds_teakra)

if(MSVC)
  # Third-party code: no warnings, and the GNU semantics melonDS expects (wrapping signed overflow).
  target_compile_options(melonds_core PRIVATE /w /clang:-fwrapv)
  target_compile_options(melonds_teakra PRIVATE /w)
  target_compile_definitions(melonds_core PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX _USE_MATH_DEFINES)
  target_include_directories(melonds_core PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat)
  # sha1.c detects endianness from POSIX headers only.
  set_source_files_properties(${MS}/sha1/sha1.c PROPERTIES COMPILE_DEFINITIONS
    "LITTLE_ENDIAN=1234;BIG_ENDIAN=4321;PDP_ENDIAN=3412;BYTE_ORDER=1234")
else()
  target_compile_options(melonds_core PRIVATE -w -fwrapv)
  target_compile_options(melonds_teakra PRIVATE -w)
endif()
if(WIN32)
  target_link_libraries(melonds_core PRIVATE ole32 comctl32 wsock32 ws2_32)
else()
  find_package(Threads REQUIRED)
  target_link_libraries(melonds_core PRIVATE Threads::Threads)
  target_link_libraries(melonds_teakra PRIVATE Threads::Threads)
endif()

# ---- SDL3 (zlib licence): window, input and audio for the game executable ----
# Downloaded at configure time into the build directory (not vendored).
if(FFCC_BUILD_GAME)
  include(FetchContent)
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
  set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG release-3.4.18
    GIT_SHALLOW TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(SDL3)
endif()
