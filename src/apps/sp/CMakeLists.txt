add_executable(sp main.cpp)

target_link_libraries(sp
  PRIVATE agentui   # PUBLIC-links agentcore
  PRIVATE ftxui::component
  PRIVATE ftxui::dom
  PRIVATE ftxui::screen
  PRIVATE CLI11::CLI11
)
