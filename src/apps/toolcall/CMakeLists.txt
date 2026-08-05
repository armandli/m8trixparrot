add_executable(toolcall main.cpp)

target_link_libraries(toolcall
  PRIVATE agentcore
  PRIVATE CLI11::CLI11
)
