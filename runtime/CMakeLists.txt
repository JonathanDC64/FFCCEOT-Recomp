# ntr runtime: services for recompiled ARM9 code on top of the melonDS core.
add_library(ntr_runtime STATIC
  src/runtime.cpp
  src/swi.cpp
  src/nitro.cpp
  src/fiber.cpp
  src/gl_between.cpp
  src/gx_match.cpp
  src/gx_stream.cpp
  src/melon_platform.cpp)
target_include_directories(ntr_runtime PUBLIC include PRIVATE src)
target_link_libraries(ntr_runtime PUBLIC melonds_core ntr_common)
