add_executable(tool_find main.cpp)

target_link_libraries(tool_find
  PRIVATE agentcore
  PRIVATE CLI11::CLI11
)
