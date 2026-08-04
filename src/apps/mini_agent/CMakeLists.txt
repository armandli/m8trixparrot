add_executable(mini_agent main.cpp)

target_link_libraries(mini_agent
  PRIVATE agentcore
  PRIVATE ftxui::component
  PRIVATE ftxui::dom
  PRIVATE ftxui::screen
  PRIVATE CLI11::CLI11
)
