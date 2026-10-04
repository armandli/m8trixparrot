include(GoogleTest)

# Live end-to-end tests: a real Agent turn driven by a real Ollama model, over
# m8's bash-only tool set. Not part of `make test` — labelled `integration` and
# run via `make integration-test`. Each test self-skips when Ollama, the model,
# or the network is unavailable.
add_executable(integration_tests
  agent_bash_integration_test.cpp
  mcp_integration_test.cpp
  websearch_integration_test.cpp
)

target_include_directories(integration_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_SOURCE_DIR}/test/unit          # reuse tool_test_env.h
)

# websearch_integration_test.cpp reads the repo's .m8/parallel_api_key when
# PARALLEL_API_KEY isn't in the environment; ctest's cwd is the build tree, so it
# needs the source path. M8_BINARY_DIR lets the bash suite put this build's
# tool_* binaries on PATH instead of relying on `make install`.
target_compile_definitions(integration_tests PRIVATE
  M8_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
  M8_BINARY_DIR="${CMAKE_BINARY_DIR}")

target_link_libraries(integration_tests
  PRIVATE agentcore                      # brings CURL::libcurl
  PRIVATE GTest::gtest_main
)

gtest_discover_tests(integration_tests PROPERTIES LABELS integration)
