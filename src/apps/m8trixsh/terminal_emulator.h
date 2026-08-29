#ifndef M8TRIXSH_TERMINAL_EMULATOR_H
#define M8TRIXSH_TERMINAL_EMULATOR_H

#include <cstdint>

#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <vterm.h>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/box.hpp>

namespace m8sh {

// A VT100/xterm terminal emulator over libvterm. It owns the parser and the
// cell grid; it has no I/O of its own. Pair it with a ShellSession: feed()
// takes the pty's output, on_pty_write emits the bytes the child should
// receive. All methods must be called from the one (UI) thread.
struct TerminalEmulator {
  TerminalEmulator(int cols, int rows);
  ~TerminalEmulator();

  TerminalEmulator(const TerminalEmulator&) = delete;
  TerminalEmulator& operator=(const TerminalEmulator&) = delete;

  // Bytes the child should receive (key/mouse encodings, terminal replies).
  std::function<void(std::string_view)> on_pty_write;
  // OSC 7: the child announced a working directory (already percent-decoded).
  std::function<void(std::string)> on_osc_cwd;
  // OSC 0/2: the child set the window title.
  std::function<void(std::string)> on_title;
  // The child rang the bell.
  std::function<void()> on_bell;

  // Feed raw pty output through the parser.
  void feed(std::string_view bytes);

  void set_size(int cols, int rows);
  int cols() const { return mCols; }
  int rows() const { return mRows; }

  // Translate one FTXUI event to child-bound bytes. Returns true if consumed.
  bool handle_key(const ftxui::Event& event);
  // Send bytes straight to the child (Ctrl-C, unrecognised escapes, ...).
  void write_raw(std::string_view bytes);
  // row/col are 0-based within the grid; button 1..3, wheel 4..5.
  void handle_mouse(int row, int col, int button, bool pressed, bool motion,
                    int mod);
  void paste(std::string_view utf8);

  bool mouse_reporting() const { return mMouseMode != 0; }
  bool alt_screen() const { return mAltScreen; }

  // Scrollback view: 0 = live screen, >0 = that many lines back.
  void scroll_view(int delta_lines);
  void reset_view();
  int view_offset() const { return mViewOffset; }
  int scrollback_len() const { return static_cast<int>(mScrollback.size()); }

  struct RenderCell {
    std::string text = " ";  // one grapheme
    bool wide = false;
    bool bold = false;
    bool dim = false;
    bool italic = false;
    bool underline = false;
    bool blink = false;
    bool reverse = false;
    bool strike = false;
    bool default_fg = true;
    bool default_bg = true;
    std::uint8_t fr = 0, fg = 0, fb = 0;
    std::uint8_t br = 0, bg = 0, bb = 0;
  };

  // The visible grid, rows*cols row-major. Rebuilt on demand.
  const std::vector<RenderCell>& grid();

  struct Cursor {
    int row = 0;
    int col = 0;
    bool visible = true;
    int shape = 1;  // VTERM_PROP_CURSORSHAPE: 1 block, 2 underline, 3 bar
    bool blink = true;
  };
  Cursor cursor() const { return mCursor; }

 protected:
  RenderCell to_render_cell(const VTermScreenCell& cell) const;
  void rebuild_grid();
  void push_scrollback(int cols, const VTermScreenCell* cells);

  // libvterm callback trampolines (user == this).
  static void output_trampoline(const char* s, size_t len, void* user);
  static int damage_trampoline(VTermRect rect, void* user);
  static int movecursor_trampoline(VTermPos pos, VTermPos oldpos, int visible,
                                   void* user);
  static int settermprop_trampoline(VTermProp prop, VTermValue* val, void* user);
  static int bell_trampoline(void* user);
  static int sb_pushline_trampoline(int cols, const VTermScreenCell* cells,
                                    void* user);
  static int sb_popline_trampoline(int cols, VTermScreenCell* cells, void* user);
  static int resize_trampoline(int rows, int cols, void* user);
  static int osc_trampoline(int command, VTermStringFragment frag, void* user);

 private:
  VTerm* mVt = nullptr;
  VTermScreen* mScreen = nullptr;
  VTermState* mState = nullptr;
  // libvterm keeps the pointers we hand it, so these live with the object.
  VTermScreenCallbacks mScreenCallbacks{};
  VTermStateFallbacks mFallbacks{};
  int mCols = 0;
  int mRows = 0;

  std::vector<RenderCell> mGrid;
  bool mGridDirty = true;

  std::deque<std::vector<RenderCell>> mScrollback;
  int mViewOffset = 0;

  Cursor mCursor;
  bool mAltScreen = false;
  int mMouseMode = 0;

  std::string mTitleAccum;
  std::string mOscAccum;
  int mOscCommand = -1;
};

// An FTXUI element that paints `emu`'s grid into the screen. `on_resize` is
// called (on the UI thread, from the layout pass) whenever the element's box
// changes size; wire it to resize both the emulator and the pty. `*out_box`
// receives the element's rectangle for mouse hit-testing. When `focused`, the
// terminal's own cursor is drawn.
ftxui::Element terminal_element(
    TerminalEmulator& emu, std::function<void(int cols, int rows)> on_resize,
    ftxui::Box* out_box, bool focused);

}  // namespace m8sh

#endif  // M8TRIXSH_TERMINAL_EMULATOR_H
