# Host build. On the ESP32, protocol/ and core/ are added individually as
# ESP-IDF components instead — see their own CMakeLists.txt for the dual-mode
# pattern that makes the same sources build both ways.
cmake_minimum_required(VERSION 3.16)
project(aquacontrol_core C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

option(AQUA_SANITIZE "Build tests with AddressSanitizer and UBSan" ON)

# -Werror on purpose: this library ends up on a device that switches a 300 W
# heater next to a tank of live animals. A warning here is a defect.
add_compile_options(-Wall -Wextra -Werror -Wshadow -Wstrict-prototypes)

if(AQUA_SANITIZE AND NOT MSVC)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

enable_testing()

add_subdirectory(protocol)
add_subdirectory(core)
add_subdirectory(tests)
