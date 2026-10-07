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
  DEPENDS ntr-recomp ${FFCC_GAME_CONFIG}/functions.txt "${FFCC_ROM}"
  COMMENT "Recompiling game code from your ROM"
  VERBATIM)

add_library(ffcc_game_code STATIC ${FFCC_GEN_SOURCES})
target_include_directories(ffcc_game_code PUBLIC ${FFCC_GEN_DIR} ${CMAKE_SOURCE_DIR}/runtime/include)
if(MSVC)
  target_compile_options(ffcc_game_code PRIVATE /w /O2)
else()
  target_compile_options(ffcc_game_code PRIVATE -w -O2)
endif()
