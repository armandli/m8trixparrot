include(GoogleTest)

# One binary over every tool's tests. They share the temp-directory fixture and
# run fast enough that splitting them per tool would only cost link time.
add_executable(unit_tests
  loopback_server.cpp
  tools_file_test.cpp
  tools_python_test.cpp
  tools_search_test.cpp
  tools_shell_test.cpp
  tools_web_test.cpp
)

target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(unit_tests
  PRIVATE agentcore
  PRIVATE GTest::gtest_main
)

# One ctest entry per TEST, so a failure names the behavior that broke rather
# than just "unit_tests".
gtest_discover_tests(unit_tests)
