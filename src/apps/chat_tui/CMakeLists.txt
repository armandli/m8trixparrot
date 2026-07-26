add_executable(chat_tui main.cpp)

target_link_libraries(chat_tui
  PRIVATE agentcore
  PRIVATE ftxui::component
  PRIVATE ftxui::dom
  PRIVATE ftxui::screen
  PRIVATE CLI11::CLI11
)
