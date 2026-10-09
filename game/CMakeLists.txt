# Recompiled game code (dev mode). Generated from the user's own ROM at build time; never committed.
if(NOT FFCC_ROM)
  message(STATUS "FFCC_ROM not set: skipping recompiled game code (pass -DFFCC_ROM=<your dump.nds>)")
  return()
endif()

set(FFCC_GAME_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/config/cfie)
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
  COMMAND ntr-recomp --rom "${FFCC_ROM}" --config "${FFCC_GAME_CONFIG}" --out "${FFCC_GEN_DIR}"
          --report "${CMAKE_BINARY_DIR}/recomp_report.txt"
  DEPENDS ntr-recomp ${FFCC_GAME_CONFIG}/functions.txt ${FFCC_GAME_CONFIG}/hooks.txt ${FFCC_GAME_CONFIG}/regions.txt "${FFCC_ROM}"
  COMMENT "Recompiling game code from your ROM"
  VERBATIM)

add_library(ffcc_game_code STATIC ${FFCC_GEN_SOURCES})
target_include_directories(ffcc_game_code PUBLIC ${FFCC_GEN_DIR} ${CMAKE_SOURCE_DIR}/runtime/include)
if(MSVC)
  target_compile_options(ffcc_game_code PRIVATE /w /O2)
else()
  target_compile_options(ffcc_game_code PRIVATE -w -O2)
endif()
target_link_libraries(ffcc_game_code PUBLIC ntr_runtime)
option(FFCC_TRACE_ENTRY "Record every recompiled function entry (slow; for debugging crashes)" OFF)
if(FFCC_TRACE_ENTRY)
  target_compile_definitions(ffcc_game_code PRIVATE NTR_TRACE_ENTRY)
endif()

# Settings menu documents and fonts, compiled into the executable.
set(FFCC_UI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/ui)
set(FFCC_UI_FILES
  ${FFCC_UI_DIR}/menu.rml
  ${FFCC_UI_DIR}/menu.rcss
  ${FFCC_UI_DIR}/crash.rml
  ${FFCC_UI_DIR}/crash.rcss
  ${FFCC_UI_DIR}/images/chevron.tga
  ${FFCC_UI_DIR}/fonts/Cinzel.ttf
  ${FFCC_UI_DIR}/fonts/AlegreyaSans-Regular.ttf
  ${FFCC_UI_DIR}/fonts/AlegreyaSans-Bold.ttf)
set(FFCC_UI_ASSETS ${CMAKE_CURRENT_BINARY_DIR}/ui_assets.cpp)
add_custom_command(
  OUTPUT ${FFCC_UI_ASSETS}
  COMMAND ${CMAKE_COMMAND} -D BASE_DIR=${FFCC_UI_DIR} -D OUTPUT=${FFCC_UI_ASSETS} "-DFILES=${FFCC_UI_FILES}"
          -P ${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake
  DEPENDS ${FFCC_UI_FILES} ${CMAKE_SOURCE_DIR}/cmake/EmbedFiles.cmake
  COMMENT "Embedding the settings menu"
  VERBATIM)

# The game executable: takes the user's ROM on the command line.
add_executable(ffcc src/main.cpp src/crash_report.cpp src/crash_dialog.cpp src/report.cpp src/video.cpp src/cfie_hooks.cpp src/settings.cpp src/input.cpp src/ui.cpp ${FFCC_UI_ASSETS})
target_include_directories(ffcc PRIVATE src)
target_link_libraries(ffcc PRIVATE ffcc_game_code ntr_runtime ntr_common SDL3::SDL3-static rmlui_backend)
if(WIN32)
  # Flip-model presentation: OpenGL drawn into a Direct3D 11 swapchain.
  target_sources(ffcc PRIVATE src/flip_present.cpp)
  target_link_libraries(ffcc PRIVATE d3d11 dxgi opengl32 dbghelp user32 advapi32 shell32)
endif()

# The version in problem reports.
find_package(Git QUIET)
set(FFCC_GIT_REV "")
if(GIT_FOUND)
  execute_process(COMMAND ${GIT_EXECUTABLE} describe --always --dirty
                  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR} OUTPUT_VARIABLE FFCC_GIT_REV OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET)
endif()
target_compile_definitions(ffcc PRIVATE FFCC_VERSION="${PROJECT_VERSION}" FFCC_GIT_REV="${FFCC_GIT_REV}")
