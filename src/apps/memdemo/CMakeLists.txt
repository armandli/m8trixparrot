add_executable(memdemo main.cpp)

target_link_libraries(memdemo
  PRIVATE agentcore
  PRIVATE CLI11::CLI11
)
