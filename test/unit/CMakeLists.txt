include(GoogleTest)

# One binary over every tool's tests. They share the temp-directory fixture and
# run fast enough that splitting them per tool would only cost link time.
add_executable(unit_tests
  loopback_server.cpp
  tools_python_test.cpp
)

target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(unit_tests
  PRIVATE agentcore
  PRIVATE GTest::gtest_main
)

# One ctest entry per TEST, so a failure names the behavior that broke rather
# than just "unit_tests".
gtest_discover_tests(unit_tests)

# Core-level tests: the agent pool (threads, the OllamaClient singleton) and the
# session store. Kept out of unit_tests because those tools chdir into a temp
# directory and rely on running sequentially.
add_executable(core_tests
  loopback_server.cpp
  agent_pool_test.cpp
  agent_context_test.cpp
  ollama_client_test.cpp
  session_store_test.cpp
)

target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(core_tests
  PRIVATE agentcore
  PRIVATE GTest::gtest_main
)

gtest_discover_tests(core_tests)
