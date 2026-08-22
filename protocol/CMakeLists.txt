# Dual-mode: an ESP-IDF component on-target, a plain static library on the host.
# This one pattern is what lets the same source be unit-tested on a laptop.
if(ESP_PLATFORM)
  idf_component_register(SRCS "src/protocol.c" INCLUDE_DIRS "include")
else()
  add_library(aqua_protocol STATIC src/protocol.c)
  target_include_directories(aqua_protocol PUBLIC include)
endif()
