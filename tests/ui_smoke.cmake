# Runs the program's UI on its own (no ROM, a scratch data folder) through tests/ui_smoke.txt. Needs a display and
# OpenGL 3.3: without them the test is skipped.
set(work "${CMAKE_CURRENT_BINARY_DIR}/ui_smoke")
file(REMOVE_RECURSE "${work}")
file(MAKE_DIRECTORY "${work}")
execute_process(
  COMMAND "${FFCC}" --prelaunch --first-run --no-settings --data-dir "${work}/data"
          --ui-script "${SCRIPT}" --ui-shots "${work}/shots"
  RESULT_VARIABLE result
  ERROR_VARIABLE err
  OUTPUT_VARIABLE out
  TIMEOUT 120)
if(err MATCHES "cannot open the game window|renderer could not start|SDL_Init failed")
  message(STATUS "no display or OpenGL here: skipped")
  return()
endif()
if(NOT result EQUAL 0)
  message(FATAL_ERROR "the UI run ended with ${result}\n${err}${out}")
endif()
string(REGEX MATCHALL "ui: [^\n]*" warnings "${err}")
if(warnings)
  string(REPLACE ";" "\n" shown "${warnings}")
  message(FATAL_ERROR "UI warnings:\n${shown}")
endif()
