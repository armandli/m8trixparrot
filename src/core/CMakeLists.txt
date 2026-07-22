add_library(agentcore STATIC
  ollama_client.cpp
)

target_include_directories(agentcore PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(agentcore
  PUBLIC CURL::libcurl
  PUBLIC nlohmann_json::nlohmann_json
)
