set(FFCC_GAME_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/config/cfie)
set(FFCC_GAME_CONFIG_EUR ${CMAKE_CURRENT_SOURCE_DIR}/config/cfip)

# Development: the game built from your ROM at build time (cfie_game, next to the executable). It is generated from the
# user's own ROM and never committed. Without FFCC_ROM nothing of the game is built, which is what a release is: the
# program then sets the game up from the player's ROM when it first runs (src/setup.cpp).
if(FFCC_ROM)
  # The configuration for the ROM's release, from the game code in its header (CFIP: Europe, otherwise USA).
  file(READ "${FFCC_ROM}" _rom_code OFFSET 12 LIMIT 4 HEX)
  if(_rom_code STREQUAL "43464950")
    set(FFCC_ROM_CONFIG ${FFCC_GAME_CONFIG_EUR})
  else()
    set(FFCC_ROM_CONFIG ${FFCC_GAME_CONFIG})
  endif()
  set(FFCC_GEN_DIR ${CMAKE_BINARY_DIR}/recomp_generated)
  set(FFCC_GEN_SHARDS 64)

  set(FFCC_GEN_SOURCES ${FFCC_GEN_DIR}/func_table.c)
  math(EXPR _last "${FFCC_GEN_SHARDS} - 1")
  foreach(i RANGE ${_last})
    string(LENGTH "${i}" _len)
    if(_len EQUAL 1)
      set(i "00${i}")
    elseif(_len EQUAL 2)
      set(i "0${i}")
    endif()
    list(APPEND FFCC_GEN_SOURCES ${FFCC_GEN_DIR}/funcs_${i}.c)
  endforeach()

  add_custom_command(
    OUTPUT ${FFCC_GEN_SOURCES} ${FFCC_GEN_DIR}/funcs.h
    COMMAND ntr-recomp --rom "${FFCC_ROM}" --config "${FFCC_ROM_CONFIG}" --out "${FFCC_GEN_DIR}"
            --report "${CMAKE_BINARY_DIR}/recomp_report.txt"
    DEPENDS ntr-recomp ${FFCC_ROM_CONFIG}/functions.txt ${FFCC_ROM_CONFIG}/hooks.txt ${FFCC_ROM_CONFIG}/regions.txt "${FFCC_ROM}"
    COMMENT "Recompiling game code from your ROM"
    VERBATIM)

  # The recompiled game is a shared library of its own, loaded by the executable (src/game_module.cpp). It is built
  # freestanding: the runtime's services come in through a table when it is loaded (ntr/recomp.h), so it links
  # against nothing. That is also how setup will build it on the player's PC, from the generated sources alone.
  add_library(cfie_game MODULE ${FFCC_GEN_SOURCES})
  target_include_directories(cfie_game PRIVATE ${FFCC_GEN_DIR} ${CMAKE_SOURCE_DIR}/runtime/include)
  target_compile_definitions(cfie_game PRIVATE NTR_GAME_MODULE=1)
  set_target_properties(cfie_game PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    LIBRARY_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
    C_VISIBILITY_PRESET hidden)
  set(FFCC_GAME_OPT "" CACHE STRING "Optimization flag for the recompiled game code (default /O1 or -O1; /Od /O2 or -O0 -O2 for benchmarks)")
  if(MSVC)
    if(NOT FFCC_GAME_OPT)
      set(FFCC_GAME_OPT /O1)
    endif()
    target_compile_options(cfie_game PRIVATE /w /GS- /Zl ${FFCC_GAME_OPT})
    target_link_options(cfie_game PRIVATE /NODEFAULTLIB /NOENTRY)
  else()
    if(NOT FFCC_GAME_OPT)
      set(FFCC_GAME_OPT -O1)
    endif()
    target_compile_options(cfie_game PRIVATE -w -ffreestanding -fno-stack-protector ${FFCC_GAME_OPT})
    target_link_options(cfie_game PRIVATE -nostdlib)
  endif()
  option(FFCC_TRACE_ENTRY "Record every recompiled function entry (slow; for debugging crashes)" OFF)
  if(FFCC_TRACE_ENTRY)
    target_compile_definitions(cfie_game PRIVATE NTR_TRACE_ENTRY)
  endif()
else()
  message(STATUS "FFCC_ROM not set: not building the game module here (the program sets the game up from a ROM when it runs)")
endif()

# The interface's styles, fonts and backdrop, compiled into the executable (game/ui/res, after Dusklight's res/).
set(FFCC_UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ui)
file(GLOB FFCC_UI_STYLES CONFIGURE_DEPENDS ${FFCC_UI_DIR}/res/rml/*.rcss)
set(FFCC_UI_FILES
  ${FFCC_UI_STYLES}
  ${FFCC_UI_DIR}/res/fonts/Cinzel.ttf
  ${FFCC_UI_DIR}/res/fonts/AlegreyaSans-Regular.ttf
  ${FFCC_UI_DIR}/res/fonts/AlegreyaSans-Bold.ttf
  ${FFCC_UI_DIR}/res/fonts/MaterialSymbolsRounded-Regular.ttf
  ${FFCC_UI_DIR}/res/prelaunch-bg.png
  ${FFCC_UI_DIR}/res/ring-backdrop.png
  ${FFCC_UI_DIR}/res/ring-holder.png
  ${FFCC_UI_DIR}/res/ring-holder-selected.png
  ${FFCC_UI_DIR}/res/ring-orb0.png
  ${FFCC_UI_DIR}/res/ring-orb1.png
  ${FFCC_UI_DIR}/res/ring-orb2.png
  ${FFCC_UI_DIR}/res/ring-orb3.png
  ${FFCC_UI_DIR}/res/ring-orb4.png
  ${FFCC_UI_DIR}/res/ring-orb5.png
  # What setup needs to build the game on the player's PC (src/setup.cpp): the game's configuration (addresses only) and
  # the header the generated code includes. They are named relative to game/ui, hence the ../ paths.
  ${FFCC_GAME_CONFIG}/functions.txt
  ${FFCC_GAME_CONFIG}/hooks.txt
  ${FFCC_GAME_CONFIG}/regions.txt
  ${FFCC_GAME_CONFIG_EUR}/functions.txt
  ${FFCC_GAME_CONFIG_EUR}/hooks.txt
  ${FFCC_GAME_CONFIG_EUR}/regions.txt
  ${CMAKE_SOURCE_DIR}/runtime/include/ntr/recomp.h)
set(FFCC_UI_ASSETS ${CMAKE_CURRENT_BINARY_DIR}/ui_assets.cpp)
add_custom_command(
  OUTPUT ${FFCC_UI_ASSETS}
  COMMAND ${CMAKE_COMMAND} -D BASE_DIR=${FFCC_UI_DIR} -D OUTPUT=${FFCC_UI_ASSETS} "-DFILES=${FFCC_UI_FILES}"
          -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake
  DEPENDS ${FFCC_UI_FILES} ${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake
  COMMENT "Embedding the interface's styles, fonts and backdrop"
  VERBATIM)

# The interface: a port of Dusklight's UI (src/ui).
file(GLOB FFCC_UI_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/*.cpp)

# The game executable: takes the user's ROM on the command line.
add_executable(ffcc src/main.cpp src/game_module.cpp src/process.cpp src/setup.cpp src/crash_report.cpp src/crash_dialog.cpp src/report.cpp src/video.cpp src/cfie_hooks.cpp src/settings.cpp src/input.cpp src/app_icon.cpp src/input_script.cpp src/spell_ring.cpp src/thp.cpp src/wii_cutscenes.cpp src/release_info.cpp src/update_check.cpp ${FFCC_UI_SOURCES} ${FFCC_UI_ASSETS})
target_include_directories(ffcc PRIVATE src)
target_include_directories(ffcc SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party/stb)  # stb_image: the Wii cutscenes' JPEG frames
if(FFCC_ROM)
  add_dependencies(ffcc cfie_game)  # (built together, next to the executable)
endif()
target_link_libraries(ffcc PRIVATE ntr_recomp ntr_runtime ntr_common SDL3::SDL3-static rmlui_backend)
if(NOT MSVC)
  # melonDS calls the Platform:: functions that ntr_runtime defines, and ntr_runtime calls melonDS: GNU ld reads each static
  # library once, so those libraries are rescanned until nothing is left undefined (link.exe does not care about the order).
  target_link_libraries(ffcc PRIVATE "$<LINK_GROUP:RESCAN,ntr_runtime,melonds_core,melonds_net>")
endif()
if(WIN32)
  # Flip-model presentation: OpenGL drawn into a Direct3D 11 swapchain.
  target_sources(ffcc PRIVATE src/flip_present.cpp)
  # The built-in icon (game/res/ffcc.ico).
  enable_language(RC)
  target_sources(ffcc PRIVATE ffcc.rc)
  set_source_files_properties(ffcc.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/res/ffcc.ico)
  target_link_libraries(ffcc PRIVATE d3d11 dxgi opengl32 dbghelp user32 advapi32 shell32 winhttp)
  # A windowed program: started from the desktop it has no console window behind it. Started from a terminal it
  # attaches to that terminal (report::attach_parent_console), so command line output still shows.
  target_link_options(ffcc PRIVATE /SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup)
endif()

# The version in problem reports.
find_package(Git QUIET)
set(FFCC_GIT_REV "")
# (--tags: the release tags are lightweight. A package is built from a tag, so it is never "dirty": in the Linux build the mounted
# tree always looks changed.)
set(FFCC_DESCRIBE_FLAGS --tags --always)
if(NOT FFCC_PACKAGE)
  list(APPEND FFCC_DESCRIBE_FLAGS --dirty)
endif()
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe ${FFCC_DESCRIBE_FLAGS}
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE FFCC_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
endif()
# A build of exactly the release tag is just the version, with no revision after it.
if(FFCC_GIT_REV STREQUAL "v${PROJECT_VERSION}-alpha")
  set(FFCC_GIT_REV "")
endif()
target_compile_definitions(ffcc PRIVATE FFCC_VERSION="${PROJECT_VERSION}-alpha" FFCC_GIT_REV="${FFCC_GIT_REV}")
