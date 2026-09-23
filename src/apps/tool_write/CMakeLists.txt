add_executable(tool_write main.cpp)

target_link_libraries(tool_write
  PRIVATE agentcore
  PRIVATE CLI11::CLI11
)
