add_executable(m8trixsh
  main.cpp
  terminal_emulator.cpp
)

target_link_libraries(m8trixsh
  PRIVATE agentui   # PUBLIC-links agentcore
  PRIVATE ftxui::component
  PRIVATE ftxui::dom
  PRIVATE ftxui::screen
  PRIVATE CLI11::CLI11
  PRIVATE PkgConfig::LIBVTERM
)

include(GoogleTest)

add_executable(m8trixsh_terminal_test
  terminal_emulator_test.cpp
  terminal_emulator.cpp
)

target_link_libraries(m8trixsh_terminal_test
  PRIVATE ftxui::component
  PRIVATE ftxui::dom
  PRIVATE ftxui::screen
  PRIVATE PkgConfig::LIBVTERM
  PRIVATE GTest::gtest_main
)

gtest_discover_tests(m8trixsh_terminal_test)
