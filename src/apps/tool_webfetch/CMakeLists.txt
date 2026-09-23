add_executable(tool_webfetch main.cpp)

target_link_libraries(tool_webfetch
  PRIVATE agentcore
  PRIVATE CLI11::CLI11
)
