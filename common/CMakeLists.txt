add_library(ntr_common STATIC
  src/blz.cpp
  src/rom.cpp
  src/sha1.cpp
  src/known_roms.cpp
)
target_include_directories(ntr_common PUBLIC include)
