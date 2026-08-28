add_executable(m8trixparrot main.cpp)

target_link_libraries(m8trixparrot
  PRIVATE agentcore
  PRIVATE ftxui::component
  PRIVATE ftxui::dom
  PRIVATE ftxui::screen
  PRIVATE CLI11::CLI11
)
