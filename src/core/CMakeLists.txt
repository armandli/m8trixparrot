add_library(agentcore STATIC
  agent.cpp
  agent_pool.cpp
  agent_settings.cpp
  sane_policy.cpp
  json_util.cpp
  basic_ollama_client.cpp
  ollama_client.cpp
  package_installer.cpp
  policy.cpp
  session_store.cpp
  shell_session.cpp
  skills.cpp
  tools_ask_user.cpp
  tools_file.cpp
  tools_package_install.cpp
  tools_shell.cpp
  tools_python.cpp
  tools_subagent.cpp
  tools_util.cpp
  tools_websearch.cpp
  workspace_context.cpp
)

target_include_directories(agentcore PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(agentcore
  PUBLIC CURL::libcurl
  PUBLIC simdjson::simdjson
  PUBLIC PkgConfig::LIBGIT2
  PUBLIC pybind11::embed
)
