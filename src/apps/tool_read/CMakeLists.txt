add_executable(tool_read main.cpp)

target_link_libraries(tool_read
  PRIVATE agentcore
  PRIVATE CLI11::CLI11
)
