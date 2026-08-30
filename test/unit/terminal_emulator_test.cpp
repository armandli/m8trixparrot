// The libvterm integration: what feed() puts in the grid, what keys/OSC produce.

#include <terminal_emulator.h>

#include <string>

#include <gtest/gtest.h>

#include <ftxui/component/event.hpp>

namespace m8sh {
namespace {

std::string cell_text(TerminalEmulator& emu, int row, int col) {
  const auto& grid = emu.grid();
  const size_t idx = static_cast<size_t>(row) * emu.cols() + col;
  return idx < grid.size() ? grid[idx].text : std::string();
}

TEST(TerminalEmulatorTest, PlainTextLandsInTheGrid) {
  TerminalEmulator emu(20, 5);
  emu.feed("\x1b[2J\x1b[H");
  emu.feed("hello");

  EXPECT_EQ("h", cell_text(emu, 0, 0));
  EXPECT_EQ("e", cell_text(emu, 0, 1));
  EXPECT_EQ("l", cell_text(emu, 0, 2));
  EXPECT_EQ("l", cell_text(emu, 0, 3));
  EXPECT_EQ("o", cell_text(emu, 0, 4));
}

TEST(TerminalEmulatorTest, SgrSetsANonDefaultForeground) {
  TerminalEmulator emu(20, 5);
  emu.feed("\x1b[31mR\x1b[0m");

  const auto& grid = emu.grid();
  ASSERT_FALSE(grid.empty());
  EXPECT_EQ("R", grid[0].text);
  EXPECT_FALSE(grid[0].default_fg);
  // Palette index 1 is a red: the red channel dominates.
  EXPECT_GT(grid[0].fr, grid[0].fg);
  EXPECT_GT(grid[0].fr, grid[0].fb);
}

TEST(TerminalEmulatorTest, AltScreenToggles) {
  TerminalEmulator emu(20, 5);
  EXPECT_FALSE(emu.alt_screen());
  emu.feed("\x1b[?1049h");
  EXPECT_TRUE(emu.alt_screen());
  emu.feed("\x1b[?1049l");
  EXPECT_FALSE(emu.alt_screen());
}

TEST(TerminalEmulatorTest, Osc7ReportsTheDecodedPath) {
  TerminalEmulator emu(20, 5);
  std::string got;
  emu.on_osc_cwd = [&got](std::string p) { got = std::move(p); };

  emu.feed("\x1b]7;file://host/tmp/a%20b\x1b\\");
  EXPECT_EQ("/tmp/a b", got);
}

TEST(TerminalEmulatorTest, Osc5171DeliversTheBase64DecodedSubmittedLine) {
  TerminalEmulator emu(20, 5);
  std::string got;
  bool line_fired = false;
  bool cwd_fired = false;
  emu.on_line_submit = [&](std::string s) {
    got = std::move(s);
    line_fired = true;
  };
  emu.on_osc_cwd = [&](std::string) { cwd_fired = true; };

  // base64("git log --oneline -5") with an embedded newline the shell's
  // `base64 | tr -d '\n'` would normally strip - the decoder skips it anyway.
  emu.feed("\x1b]5171;Z2l0IGxvZyAt\nLW9uZWxpbmUgLTU=\x07");

  EXPECT_TRUE(line_fired);
  EXPECT_FALSE(cwd_fired);
  EXPECT_EQ("git log --oneline -5", got);
}

TEST(TerminalEmulatorTest, ArrowKeyEncodesToTheChild) {
  TerminalEmulator emu(20, 5);
  std::string sent;
  emu.on_pty_write = [&sent](std::string_view b) { sent.append(b); };

  emu.handle_key(ftxui::Event::ArrowUp);
  EXPECT_EQ("\x1b[A", sent);

  sent.clear();
  emu.handle_key(ftxui::Event::Character("x"));
  EXPECT_EQ("x", sent);

  sent.clear();
  emu.handle_key(ftxui::Event::Return);
  EXPECT_EQ("\r", sent);
}

TEST(TerminalEmulatorTest, MouseReportingIsOffUntilTheChildEnablesIt) {
  TerminalEmulator emu(20, 5);
  EXPECT_FALSE(emu.mouse_reporting());
  emu.feed("\x1b[?1000h");
  EXPECT_TRUE(emu.mouse_reporting());
}

TEST(TerminalEmulatorTest, ResizeUpdatesTheReportedSize) {
  TerminalEmulator emu(20, 5);
  emu.set_size(100, 40);
  EXPECT_EQ(100, emu.cols());
  EXPECT_EQ(40, emu.rows());
  emu.feed("hi");  // must not crash at the new size
  EXPECT_EQ("h", cell_text(emu, 0, 0));
}

}  // namespace
}  // namespace m8sh
