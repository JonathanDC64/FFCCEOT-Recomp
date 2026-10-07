cmake_minimum_required(VERSION 3.28)
project(FFCCEOTRecomp VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /wd4100 /wd4201)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Path to the user's own ROM. Only needed for steps that generate code from it (dev mode).
set(FFCC_ROM "" CACHE FILEPATH "Path to your own FFCC: Echoes of Time (USA) ROM dump")

option(FFCC_BUILD_TESTS "Build unit tests" ON)

add_subdirectory(common)
add_subdirectory(recompiler)
add_subdirectory(tools)
add_subdirectory(game)

if(FFCC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
