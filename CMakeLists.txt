cmake_minimum_required(VERSION 3.28)
# The program carries its C/C++ runtime (no Visual C++ redistributable to install), for every target including the fetched ones.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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

# A build for a release package (tools/package.py): no path of the build machine ends up in the binary or its debug info.
option(FFCC_PACKAGE "Build for a release package" OFF)
if(FFCC_PACKAGE)
  if(MSVC)
    add_compile_options("/clang:-ffile-prefix-map=${CMAKE_SOURCE_DIR}=." "/clang:-ffile-prefix-map=${CMAKE_BINARY_DIR}=.build")
    add_link_options(/Brepro "/PDBALTPATH:%_PDB%")
  else()
    add_compile_options("-ffile-prefix-map=${CMAKE_SOURCE_DIR}=." "-ffile-prefix-map=${CMAKE_BINARY_DIR}=.build")
    add_link_options(-static-libstdc++ -static-libgcc)  # (the package runs on systems with other versions of them)
  endif()
endif()

option(FFCC_BUILD_TESTS "Build unit tests" ON)
option(FFCC_BUILD_GAME "Build the runtime and game executable (needs the melonDS submodule)" ON)

add_subdirectory(common)
add_subdirectory(recompiler)
add_subdirectory(tools)
if(FFCC_BUILD_GAME)
  add_subdirectory(third_party)
  add_subdirectory(runtime)
  add_subdirectory(game)
endif()

if(FFCC_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
