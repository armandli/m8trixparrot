add_library(agentcore STATIC
  agent.cpp
  agent_pool.cpp
  sane_policy.cpp
  json_util.cpp
  basic_ollama_client.cpp
  ollama_client.cpp
  policy.cpp
  session_store.cpp
  skills.cpp
  tools_python.cpp
  tools_subagent.cpp
  tools_util.cpp
  workspace_context.cpp
)

target_include_directories(agentcore PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(agentcore
  PUBLIC CURL::libcurl
  PUBLIC simdjson::simdjson
  PUBLIC PkgConfig::LIBGIT2
  PUBLIC pybind11::embed
)
