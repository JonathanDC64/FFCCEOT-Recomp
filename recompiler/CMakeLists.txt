add_library(ntr_recomp STATIC
  src/analysis.cpp
  src/decode.cpp
  src/emit.cpp
  src/program.cpp
)
target_include_directories(ntr_recomp PUBLIC include)
target_link_libraries(ntr_recomp PUBLIC ntr_common)

add_executable(ntr-recomp src/main.cpp)
target_link_libraries(ntr-recomp PRIVATE ntr_recomp)
