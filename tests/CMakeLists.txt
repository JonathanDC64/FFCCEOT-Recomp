add_executable(test_common test_common.cpp)
target_link_libraries(test_common PRIVATE ntr_common)
add_test(NAME common COMMAND test_common)

# Frame interpolation's stream matching and blending (no melonDS or game data needed).
add_executable(test_gx test_gx.cpp ${PROJECT_SOURCE_DIR}/runtime/src/gx_match.cpp)
target_include_directories(test_gx PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)
add_test(NAME gx COMMAND test_gx)

# What the DS sees of the host's buttons (taps, mashing) against a game that reads them every second frame.
add_executable(test_input test_input.cpp)
target_include_directories(test_input PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)
add_test(NAME input COMMAND test_input)

# settings.ini round trip and the controls window's binding rules.
add_executable(test_settings test_settings.cpp ${PROJECT_SOURCE_DIR}/game/src/settings.cpp ${PROJECT_SOURCE_DIR}/game/src/input.cpp)
target_include_directories(test_settings PRIVATE ${PROJECT_SOURCE_DIR}/game/src)
target_link_libraries(test_settings PRIVATE ntr_runtime ntr_common SDL3::SDL3-static)
add_test(NAME settings COMMAND test_settings)

# The UI on its own: first run, prelaunch and the settings tabs; any "ui:" warning fails it (tests/ui_smoke.cmake).
if(TARGET ffcc)
  add_test(NAME ui_smoke COMMAND ${CMAKE_COMMAND} -DFFCC=$<TARGET_FILE:ffcc> -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/ui_smoke.txt
                  -P ${CMAKE_CURRENT_SOURCE_DIR}/ui_smoke.cmake)
endif()
