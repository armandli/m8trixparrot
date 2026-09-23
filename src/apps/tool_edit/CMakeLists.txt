add_executable(tool_edit main.cpp)

target_link_libraries(tool_edit
  PRIVATE agentcore
  PRIVATE CLI11::CLI11
)
