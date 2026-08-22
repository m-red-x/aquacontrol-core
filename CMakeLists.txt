# Host build entry point ONLY.
#
# Under ESP-IDF this file must do nothing: the components live in protocol/ and
# core/ and are discovered individually via EXTRA_COMPONENT_DIRS. If IDF ever
# processes this file as a component it would hit project() and fail, so bail
# out early rather than depending on where a firmware repo places the submodule.
if(ESP_PLATFORM)
  return()
endif()

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
