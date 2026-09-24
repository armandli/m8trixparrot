add_executable(m8trixparrot
  main.cpp
  parrot_prompt.cpp
)

target_include_directories(m8trixparrot PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(m8trixparrot
  PRIVATE agentui   # PUBLIC-links agentcore
  PRIVATE ftxui::component
  PRIVATE ftxui::dom
  PRIVATE ftxui::screen
  PRIVATE CLI11::CLI11
)
