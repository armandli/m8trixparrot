add_executable(tool_websearch main.cpp)

target_link_libraries(tool_websearch
  PRIVATE agentcore
  PRIVATE CLI11::CLI11
)
