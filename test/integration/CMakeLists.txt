include(GoogleTest)

# Live end-to-end tests: a real Agent turn driven by a real Ollama model, with a
# python-only tool set. Not part of `make test` — labelled `integration` and run
# via `make integration-test`. Each test self-skips when Ollama, the model, or
# the network is unavailable.
#
# Own main() (not GTest::gtest_main): it guards against the suite re-entering
# itself through the embedded interpreter's sys.executable, and _Exit()s past
# static teardown of C extensions. See main.cpp.
add_executable(integration_tests
  main.cpp
  agent_python_integration_test.cpp
)

target_include_directories(integration_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/test/unit          # reuse tool_test_env.h
)

target_link_libraries(integration_tests
  PRIVATE agentcore                      # brings CURL::libcurl + pybind11::embed
  PRIVATE GTest::gtest
)

gtest_discover_tests(integration_tests PROPERTIES LABELS integration)
