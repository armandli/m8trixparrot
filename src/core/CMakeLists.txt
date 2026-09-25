add_library(agentcore STATIC
  agent.cpp
  tools/bash_repl.cpp
  system_prompt.cpp
  agent_pool.cpp
  agent_settings.cpp
  policy/sane_policy.cpp
  util/json_util.cpp
  util/uuid.cpp
  memory_store.cpp
  oc/basic_ollama_client.cpp
  byte_io.cpp
  oc/ollama_client.cpp
  tools/package_installer.cpp
  policy/policy.cpp
  session_store.cpp
  tools/shell_session.cpp
  skills.cpp
  tools/tools_ask_user.cpp
  tools/tools_bash_repl.cpp
  tools/tools_bash_search.cpp
  tools/tools_file.cpp
  tools_memory.cpp
  tools/tools_package_install.cpp
  tools/tools_search.cpp
  tools/tools_shell.cpp
  tools/tools_python.cpp
  tools_subagent.cpp
  tools/tools_util.cpp
  tools/tools_web.cpp
  tools/tools_websearch.cpp
  vector_index.cpp
  vector_store.cpp
  workspace_context.cpp
)

target_include_directories(agentcore PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(agentcore
  PUBLIC CURL::libcurl
  PUBLIC simdjson::simdjson
  PUBLIC PkgConfig::LIBGIT2
  PUBLIC pybind11::embed
)
