add_library(ntr_common STATIC
  src/aes.cpp
  src/blz.cpp
  src/rom.cpp
  src/sha1.cpp
  src/known_roms.cpp
  src/wii_disc.cpp
)
target_include_directories(ntr_common PUBLIC include)
