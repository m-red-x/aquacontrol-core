# One executable per test file; ctest reports them individually.
function(aqua_add_test name)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE ${ARGN})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

aqua_add_test(test_schedule aqua_core)
aqua_add_test(test_detectors aqua_core)
# test_protocol also exercises aqua_probe_check, to prove the DS18B20 fault
# sentinels survive a round trip through the wire format and are still
# classified as faults rather than read as temperatures.
aqua_add_test(test_protocol aqua_protocol aqua_core)
