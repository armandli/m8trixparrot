include(GoogleTest)

# One binary over every tool's tests. They share the temp-directory fixture and
# run fast enough that splitting them per tool would only cost link time.
add_executable(unit_tests
  agent_settings_test.cpp
  loopback_server.cpp
  shell_session_test.cpp
  skills_test.cpp
  tools_bash_repl_test.cpp
  tools_bash_search_test.cpp
  protected_paths_test.cpp
  sane_policy_test.cpp
  tools_file_test.cpp
  tools_search_test.cpp
  tools_shell_test.cpp
  tools_util_test.cpp
  tools_web_test.cpp
  tools_websearch_test.cpp
  json_value_test.cpp
  util_codec_test.cpp
  util_url_test.cpp
)

target_include_directories(unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(unit_tests PRIVATE M8_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

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
  http_test_server.cpp
  agent_mcp_test.cpp
  agent_pool_test.cpp
  agent_context_test.cpp
  agent_skills_test.cpp
  agent_tools_test.cpp
  system_prompt_test.cpp
  ollama_client_test.cpp
  session_store_test.cpp
  vector_index_test.cpp
  vector_store_test.cpp
  memory_store_test.cpp
)

target_include_directories(core_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(core_tests
  PRIVATE agentcore
  PRIVATE GTest::gtest_main
)

gtest_discover_tests(core_tests)

# The MCP client (src/core/mcp): protocol, schema lowering, tool search, and —
# from phase 2 on — real transports against a fake server process. Its own
# binary because those tests spawn processes and run servers on threads, which
# unit_tests (it chdirs per test) does not tolerate, and they must not share
# core_tests' singletons.
# A scripted MCP server the client tests run as a real child process.
add_executable(fake_mcp_server fake_mcp_server.cpp)
target_link_libraries(fake_mcp_server PRIVATE agentcore)

add_executable(mcp_tests
  http_test_server.cpp
  mcp_client_test.cpp
  mcp_config_test.cpp
  mcp_http_test.cpp
  mcp_json_rpc_test.cpp
  mcp_registry_test.cpp
  mcp_sse_test.cpp
  mcp_schema_test.cpp
  mcp_content_test.cpp
  mcp_tool_search_test.cpp
)

target_include_directories(mcp_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_definitions(mcp_tests PRIVATE
  M8_MCP_SCHEMA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/mcp_schemas"
  M8_FAKE_MCP_SERVER="$<TARGET_FILE:fake_mcp_server>"
)
add_dependencies(mcp_tests fake_mcp_server)

target_link_libraries(mcp_tests
  PRIVATE agentcore
  PRIVATE GTest::gtest_main
)

gtest_discover_tests(mcp_tests)

# sp's system prompt, slash-command grammar and hand-driven memory operations.
# Its own binary rather than part of unit_tests for the same reason core_tests
# is: these drive a MemoryStore over a temp file and run alongside the
# vector-store tests.
add_executable(sp_tests
  sp_memory_test.cpp
  sp_prompt_test.cpp
  sp_paths_test.cpp
  ${CMAKE_SOURCE_DIR}/src/apps/sp/sp_memory.cpp
  ${CMAKE_SOURCE_DIR}/src/apps/sp/sp_prompt.cpp
  ${CMAKE_SOURCE_DIR}/src/apps/sp/sp_paths.cpp
)

target_include_directories(sp_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/apps/sp)

target_link_libraries(sp_tests
  PRIVATE agentcore
  PRIVATE GTest::gtest_main
)

gtest_discover_tests(sp_tests)

# m8's system prompt and its workspace-path resolution. Its own binary rather
# than part of unit_tests for the same reason sp_tests is: the path tests build
# a directory tree under a temp root and run sequentially.
add_executable(m8_tests
  m8_prompt_test.cpp
  m8_paths_test.cpp
  m8_mcp_cli_test.cpp
  m8_mcp_view_test.cpp
  ${CMAKE_SOURCE_DIR}/src/apps/m8/m8_prompt.cpp
  ${CMAKE_SOURCE_DIR}/src/apps/m8/m8_paths.cpp
  ${CMAKE_SOURCE_DIR}/src/apps/m8/m8_mcp_cli.cpp
  ${CMAKE_SOURCE_DIR}/src/apps/m8/m8_mcp_view.cpp
)

target_include_directories(m8_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/apps/m8)

# `m8 mcp list` and `get` start real servers; the tests point them at the
# scripted one.
target_compile_definitions(m8_tests PRIVATE
  M8_FAKE_MCP_SERVER="$<TARGET_FILE:fake_mcp_server>"
)
add_dependencies(m8_tests fake_mcp_server)

target_link_libraries(m8_tests
  PRIVATE agentcore
  PRIVATE CLI11::CLI11
  PRIVATE GTest::gtest_main
)

gtest_discover_tests(m8_tests)

# m8trixsh's system prompt. A free function over PromptFacts with no FTXUI,
# libvterm or network dependency, so it builds here regardless of whether its
# application does — which is the point of giving each application its own
# prompt translation unit.
add_executable(prompt_tests
  sh_prompt_test.cpp
  ${CMAKE_SOURCE_DIR}/src/apps/m8trixsh/sh_prompt.cpp
)

target_include_directories(prompt_tests
  PRIVATE ${CMAKE_SOURCE_DIR}/src/apps/m8trixsh
)

target_link_libraries(prompt_tests
  PRIVATE agentcore
  PRIVATE GTest::gtest_main
)

gtest_discover_tests(prompt_tests)

# m8trixsh's terminal emulator and shell integration have no core dependency,
# so they get their own binary rather than joining unit_tests. Only built when
# libvterm is available, matching the guard around m8trixsh itself in
# src/apps/CMakeLists.txt.
if(TARGET PkgConfig::LIBVTERM)
  add_executable(m8trixsh_terminal_test
    terminal_emulator_test.cpp
    ${CMAKE_SOURCE_DIR}/src/apps/m8trixsh/terminal_emulator.cpp
    shell_integration_test.cpp
    ${CMAKE_SOURCE_DIR}/src/apps/m8trixsh/shell_integration.cpp
  )

  target_include_directories(m8trixsh_terminal_test
    PRIVATE ${CMAKE_SOURCE_DIR}/src/apps/m8trixsh
  )

  target_link_libraries(m8trixsh_terminal_test
    PRIVATE ftxui::component
    PRIVATE ftxui::dom
    PRIVATE ftxui::screen
    PRIVATE PkgConfig::LIBVTERM
    PRIVATE GTest::gtest_main
  )

  gtest_discover_tests(m8trixsh_terminal_test)
endif()
