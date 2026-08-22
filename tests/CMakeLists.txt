# One executable per test file; ctest reports them individually.
function(aqua_add_test name lib)
  add_executable(${name} ${name}.c)
  target_link_libraries(${name} PRIVATE ${lib})
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME ${name} COMMAND ${name})
endfunction()

aqua_add_test(test_schedule aqua_core)
aqua_add_test(test_detectors aqua_core)
aqua_add_test(test_protocol aqua_protocol)
