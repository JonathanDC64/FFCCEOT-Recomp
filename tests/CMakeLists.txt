add_executable(test_common test_common.cpp)
target_link_libraries(test_common PRIVATE ntr_common)
add_test(NAME common COMMAND test_common)
