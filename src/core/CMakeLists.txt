add_library(agentcore STATIC
  basic_agent.cpp
  json_util.cpp
  ollama_client.cpp
  policy.cpp
  session_store.cpp
  tools_file.cpp
  tools_memory.cpp
  tools_search.cpp
  tools_shell.cpp
  tools_util.cpp
  tools_web.cpp
  tools_websearch.cpp
  workspace_context.cpp
)

target_include_directories(agentcore PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(agentcore
  PUBLIC CURL::libcurl
  PUBLIC simdjson::simdjson
  PUBLIC PkgConfig::LIBGIT2
)
