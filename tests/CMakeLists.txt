add_executable(test_common test_common.cpp)
target_link_libraries(test_common PRIVATE ntr_common)
add_test(NAME common COMMAND test_common)

# Frame interpolation's stream matching and blending (no melonDS or game data needed).
add_executable(test_gx test_gx.cpp ${PROJECT_SOURCE_DIR}/runtime/src/gx_match.cpp)
target_include_directories(test_gx PRIVATE ${PROJECT_SOURCE_DIR}/runtime/src)
add_test(NAME gx COMMAND test_gx)
