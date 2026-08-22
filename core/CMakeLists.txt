if(ESP_PLATFORM)
  idf_component_register(SRCS "src/schedule.c" "src/detectors.c"
                         INCLUDE_DIRS "include")
else()
  add_library(aqua_core STATIC src/schedule.c src/detectors.c)
  target_include_directories(aqua_core PUBLIC include)
endif()
