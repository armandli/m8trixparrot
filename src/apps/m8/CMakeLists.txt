add_executable(m8
  main.cpp
  m8_prompt.cpp
  m8_paths.cpp
)

target_include_directories(m8 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(m8
  PRIVATE agentui   # PUBLIC-links agentcore
  PRIVATE ftxui::component
  PRIVATE ftxui::dom
  PRIVATE ftxui::screen
  PRIVATE CLI11::CLI11
)
