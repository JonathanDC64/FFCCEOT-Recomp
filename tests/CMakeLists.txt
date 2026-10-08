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
