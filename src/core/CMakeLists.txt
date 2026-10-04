add_library(agentcore STATIC
  agent/agent.cpp
  tools/bash_repl.cpp
  agent/system_prompt.cpp
  agent/agent_pool.cpp
  agent/agent_settings.cpp
  policy/sane_policy.cpp
  util/base64.cpp
  util/json_util.cpp
  util/json_value.cpp
  util/process.cpp
  util/secure_random.cpp
  util/sha256.cpp
  util/open_url.cpp
  util/text.cpp
  util/url.cpp
  util/uuid.cpp
  mcp/catalog.cpp
  mcp/client.cpp
  mcp/config.cpp
  mcp/content.cpp
  mcp/credential_store.cpp
  mcp/elicitation.cpp
  mcp/http_transport.cpp
  mcp/json_rpc.cpp
  mcp/oauth.cpp
  mcp/protocol.cpp
  mcp/registry.cpp
  mcp/resource_tool.cpp
  mcp/schema.cpp
  mcp/sse.cpp
  mcp/stdio_transport.cpp
  mcp/tool_search.cpp
  mcp/trust.cpp
  vdb/memory_ops.cpp
  vdb/memory_store.cpp
  oc/basic_ollama_client.cpp
  vdb/byte_io.cpp
  oc/ollama_client.cpp
  policy/policy.cpp
  agent/session_store.cpp
  tools/protected_paths.cpp
  tools/shell_session.cpp
  agent/skills.cpp
  tools/tools_ask_user.cpp
  tools/tools_bash_repl.cpp
  tools/tools_bash_search.cpp
  tools/tools_file.cpp
  vdb/memory_tool.cpp
  tools/tools_search.cpp
  tools/tools_shell.cpp
  agent/subagent_tool.cpp
  tools/tools_util.cpp
  tools/tools_web.cpp
  tools/tools_websearch.cpp
  vdb/vector_index.cpp
  vdb/vector_store.cpp
  agent/workspace_context.cpp
)

target_include_directories(agentcore PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(agentcore
  PUBLIC CURL::libcurl
  PUBLIC simdjson::simdjson
  PUBLIC PkgConfig::LIBGIT2
)
