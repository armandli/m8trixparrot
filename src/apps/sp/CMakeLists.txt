add_executable(sp
  main.cpp
  sp_memory.cpp
)

target_include_directories(sp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(sp
  PRIVATE agentui   # PUBLIC-links agentcore
  PRIVATE ftxui::component
  PRIVATE ftxui::dom
  PRIVATE ftxui::screen
  PRIVATE CLI11::CLI11
)
