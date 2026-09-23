add_executable(tool_grep main.cpp)

target_link_libraries(tool_grep
  PRIVATE agentcore
  PRIVATE CLI11::CLI11
)
