find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

add_executable(corded_unit_tests unit/crypto_test.cpp)
target_link_libraries(corded_unit_tests PRIVATE corded_core Catch2::Catch2WithMain)
catch_discover_tests(corded_unit_tests)

# The end-to-end and terminal tests start processes and pseudo-terminals the
# POSIX way; they are not ported to Windows yet.
if(WIN32)
  return()
endif()

# End-to-end: starts a real cordedd and drives two engines through the C ABI.
add_executable(corded_integration_tests integration/e2e_test.cpp)
target_link_libraries(corded_integration_tests PRIVATE
  corded nlohmann_json::nlohmann_json Catch2::Catch2WithMain Threads::Threads)
target_compile_definitions(corded_integration_tests PRIVATE
  CORDEDD_PATH="$<TARGET_FILE:cordedd>")
add_dependencies(corded_integration_tests cordedd)
catch_discover_tests(corded_integration_tests)

# Drives two real corded-tui processes in pseudo-terminals.
if(CORDED_BUILD_TUI)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND)
    add_test(NAME tui_smoke
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tui_smoke.py ${CMAKE_BINARY_DIR}/bin)
    set_tests_properties(tui_smoke PROPERTIES TIMEOUT 180)
  endif()
endif()
