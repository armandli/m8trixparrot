#include <terminal_emulator.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <utility>

#include <ftxui/dom/node.hpp>
#include <ftxui/screen/screen.hpp>

namespace f = ftxui;

namespace m8sh {

namespace {

constexpr int kScrollbackLimit = 5000;

void append_utf8(std::string& out, std::uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

// Decodes a (valid) UTF-8 string to codepoints. Malformed bytes pass through as
// Latin-1, which is good enough for keyboard input.
std::vector<std::uint32_t> decode_utf8(std::string_view s) {
  std::vector<std::uint32_t> out;
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    std::uint32_t cp = c;
    int extra = 0;
    if ((c & 0x80) == 0) {
      extra = 0;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
    }
    if (i + extra >= s.size()) {
      out.push_back(c);
      ++i;
      continue;
    }
    for (int k = 0; k < extra; ++k) {
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + 1 + k]) & 0x3F);
    }
    out.push_back(cp);
    i += extra + 1;
  }
  return out;
}

// "file://host/percent%20encoded/path" -> "/percent encoded/path".
std::string decode_osc7_path(std::string_view uri) {
  std::string_view rest = uri;
  const size_t scheme = rest.find("://");
  if (scheme != std::string_view::npos) {
    rest.remove_prefix(scheme + 3);
    const size_t slash = rest.find('/');
    rest = slash == std::string_view::npos ? std::string_view() : rest.substr(slash);
  }
  std::string out;
  for (size_t i = 0; i < rest.size(); ++i) {
    if (rest[i] == '%' and i + 2 < rest.size()) {
      const auto hex = [](char h) -> int {
        if (h >= '0' and h <= '9') return h - '0';
        if (h >= 'a' and h <= 'f') return h - 'a' + 10;
        if (h >= 'A' and h <= 'F') return h - 'A' + 10;
        return -1;
      };
      const int hi = hex(rest[i + 1]);
      const int lo = hex(rest[i + 2]);
      if (hi >= 0 and lo >= 0) {
        out.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
        continue;
      }
    }
    out.push_back(rest[i]);
  }
  return out;
}

f::Screen::Cursor::Shape to_ftxui_shape(int vterm_shape, bool blink) {
  switch (vterm_shape) {
    default:
    case 1:  // block
      return blink ? f::Screen::Cursor::BlockBlinking
                   : f::Screen::Cursor::Block;
    case 2:  // underline
      return blink ? f::Screen::Cursor::UnderlineBlinking
                   : f::Screen::Cursor::Underline;
    case 3:  // bar
      return blink ? f::Screen::Cursor::BarBlinking : f::Screen::Cursor::Bar;
  }
}

}  // namespace

TerminalEmulator::TerminalEmulator(int cols, int rows)
    : mCols(cols > 0 ? cols : 80), mRows(rows > 0 ? rows : 24) {
  mVt = vterm_new(mRows, mCols);
  vterm_set_utf8(mVt, 1);
  vterm_output_set_callback(mVt, &TerminalEmulator::output_trampoline, this);

  mScreen = vterm_obtain_screen(mVt);
  mState = vterm_obtain_state(mVt);

  VTermColor fg;
  VTermColor bg;
  vterm_color_rgb(&fg, 0xd0, 0xd0, 0xd0);
  vterm_color_rgb(&bg, 0x00, 0x00, 0x00);
  vterm_state_set_default_colors(mState, &fg, &bg);

  mScreenCallbacks.damage = &TerminalEmulator::damage_trampoline;
  mScreenCallbacks.movecursor = &TerminalEmulator::movecursor_trampoline;
  mScreenCallbacks.settermprop = &TerminalEmulator::settermprop_trampoline;
  mScreenCallbacks.bell = &TerminalEmulator::bell_trampoline;
  mScreenCallbacks.resize = &TerminalEmulator::resize_trampoline;
  mScreenCallbacks.sb_pushline = &TerminalEmulator::sb_pushline_trampoline;
  mScreenCallbacks.sb_popline = &TerminalEmulator::sb_popline_trampoline;
  vterm_screen_set_callbacks(mScreen, &mScreenCallbacks, this);
  vterm_screen_set_damage_merge(mScreen, VTERM_DAMAGE_SCROLL);
  vterm_screen_enable_altscreen(mScreen, 1);

  mFallbacks.osc = &TerminalEmulator::osc_trampoline;
  vterm_state_set_unrecognised_fallbacks(mState, &mFallbacks, this);

  vterm_screen_reset(mScreen, 1);

  mGrid.assign(static_cast<size_t>(mRows) * mCols, RenderCell{});
}

TerminalEmulator::~TerminalEmulator() {
  if (mVt != nullptr) vterm_free(mVt);
}

void TerminalEmulator::feed(std::string_view bytes) {
  vterm_input_write(mVt, bytes.data(), bytes.size());
  vterm_screen_flush_damage(mScreen);
  mGridDirty = true;
}

void TerminalEmulator::set_size(int cols, int rows) {
  cols = cols > 0 ? cols : 1;
  rows = rows > 0 ? rows : 1;
  if (cols == mCols and rows == mRows) return;
  mCols = cols;
  mRows = rows;
  vterm_set_size(mVt, mRows, mCols);
  vterm_screen_flush_damage(mScreen);
  mViewOffset = 0;
  mGridDirty = true;
}

void TerminalEmulator::write_raw(std::string_view bytes) {
  if (on_pty_write and not bytes.empty()) on_pty_write(bytes);
}

void TerminalEmulator::paste(std::string_view utf8) {
  vterm_keyboard_start_paste(mVt);
  for (std::uint32_t cp : decode_utf8(utf8)) {
    vterm_keyboard_unichar(mVt, cp, VTERM_MOD_NONE);
  }
  vterm_keyboard_end_paste(mVt);
}

void TerminalEmulator::scroll_view(int delta_lines) {
  const int max_back = static_cast<int>(mScrollback.size());
  mViewOffset = std::clamp(mViewOffset + delta_lines, 0, max_back);
  mGridDirty = true;
}

void TerminalEmulator::reset_view() {
  if (mViewOffset != 0) {
    mViewOffset = 0;
    mGridDirty = true;
  }
}

bool TerminalEmulator::handle_key(const f::Event& e) {
  reset_view();

  const auto send_key = [&](VTermKey key, VTermModifier mod) {
    vterm_keyboard_key(mVt, key, mod);
    return true;
  };

  if (e == f::Event::Return) return send_key(VTERM_KEY_ENTER, VTERM_MOD_NONE);
  if (e == f::Event::Tab) return send_key(VTERM_KEY_TAB, VTERM_MOD_NONE);
  if (e == f::Event::TabReverse) return send_key(VTERM_KEY_TAB, VTERM_MOD_SHIFT);
  if (e == f::Event::Backspace)
    return send_key(VTERM_KEY_BACKSPACE, VTERM_MOD_NONE);
  if (e == f::Event::Delete) return send_key(VTERM_KEY_DEL, VTERM_MOD_NONE);
  if (e == f::Event::Escape) return send_key(VTERM_KEY_ESCAPE, VTERM_MOD_NONE);
  if (e == f::Event::ArrowUp) return send_key(VTERM_KEY_UP, VTERM_MOD_NONE);
  if (e == f::Event::ArrowDown) return send_key(VTERM_KEY_DOWN, VTERM_MOD_NONE);
  if (e == f::Event::ArrowLeft) return send_key(VTERM_KEY_LEFT, VTERM_MOD_NONE);
  if (e == f::Event::ArrowRight)
    return send_key(VTERM_KEY_RIGHT, VTERM_MOD_NONE);
  if (e == f::Event::ArrowUpCtrl) return send_key(VTERM_KEY_UP, VTERM_MOD_CTRL);
  if (e == f::Event::ArrowDownCtrl)
    return send_key(VTERM_KEY_DOWN, VTERM_MOD_CTRL);
  if (e == f::Event::ArrowLeftCtrl)
    return send_key(VTERM_KEY_LEFT, VTERM_MOD_CTRL);
  if (e == f::Event::ArrowRightCtrl)
    return send_key(VTERM_KEY_RIGHT, VTERM_MOD_CTRL);
  if (e == f::Event::Home) return send_key(VTERM_KEY_HOME, VTERM_MOD_NONE);
  if (e == f::Event::End) return send_key(VTERM_KEY_END, VTERM_MOD_NONE);
  if (e == f::Event::Insert) return send_key(VTERM_KEY_INS, VTERM_MOD_NONE);
  if (e == f::Event::PageUp) return send_key(VTERM_KEY_PAGEUP, VTERM_MOD_NONE);
  if (e == f::Event::PageDown)
    return send_key(VTERM_KEY_PAGEDOWN, VTERM_MOD_NONE);

  static const std::array<const f::Event*, 12> kFn = {
      &f::Event::F1, &f::Event::F2,  &f::Event::F3,  &f::Event::F4,
      &f::Event::F5, &f::Event::F6,  &f::Event::F7,  &f::Event::F8,
      &f::Event::F9, &f::Event::F10, &f::Event::F11, &f::Event::F12};
  for (int i = 0; i < 12; ++i) {
    if (e == *kFn[i]) {
      return send_key(static_cast<VTermKey>(VTERM_KEY_FUNCTION(i + 1)),
                      VTERM_MOD_NONE);
    }
  }

  if (e.is_character()) {
    for (std::uint32_t cp : decode_utf8(e.character())) {
      vterm_keyboard_unichar(mVt, cp, VTERM_MOD_NONE);
    }
    return true;
  }

  // Control bytes (Ctrl-A .. Ctrl-Z etc.) and any escape sequence FTXUI parsed
  // but we do not special-case: the bytes it received are already what the
  // child expects.
  if (not e.input().empty()) {
    write_raw(e.input());
    return true;
  }
  return false;
}

void TerminalEmulator::handle_mouse(int row, int col, int button, bool pressed,
                                    bool motion, int mod) {
  reset_view();
  const VTermModifier m = static_cast<VTermModifier>(mod);
  vterm_mouse_move(mVt, row, col, m);
  if (not motion and button > 0) {
    vterm_mouse_button(mVt, button, pressed, m);
  }
}

// --- rendering -------------------------------------------------------------

TerminalEmulator::RenderCell TerminalEmulator::to_render_cell(
    const VTermScreenCell& cell) const {
  RenderCell rc;

  std::string text;
  for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL and cell.chars[i] != 0; ++i) {
    append_utf8(text, cell.chars[i]);
  }
  rc.text = text.empty() ? std::string(" ") : std::move(text);
  rc.wide = cell.width == 2;

  rc.bold = cell.attrs.bold != 0;
  rc.italic = cell.attrs.italic != 0;
  rc.underline = cell.attrs.underline != 0;
  rc.blink = cell.attrs.blink != 0;
  rc.reverse = cell.attrs.reverse != 0;
  rc.strike = cell.attrs.strike != 0;

  VTermColor fg = cell.fg;
  VTermColor bg = cell.bg;
  if (VTERM_COLOR_IS_DEFAULT_FG(&fg)) {
    rc.default_fg = true;
  } else {
    vterm_screen_convert_color_to_rgb(mScreen, &fg);
    rc.default_fg = false;
    rc.fr = fg.rgb.red;
    rc.fg = fg.rgb.green;
    rc.fb = fg.rgb.blue;
  }
  if (VTERM_COLOR_IS_DEFAULT_BG(&bg)) {
    rc.default_bg = true;
  } else {
    vterm_screen_convert_color_to_rgb(mScreen, &bg);
    rc.default_bg = false;
    rc.br = bg.rgb.red;
    rc.bg = bg.rgb.green;
    rc.bb = bg.rgb.blue;
  }
  return rc;
}

void TerminalEmulator::rebuild_grid() {
  mGrid.assign(static_cast<size_t>(mRows) * mCols, RenderCell{});

  const int back = std::clamp(mViewOffset, 0,
                              static_cast<int>(mScrollback.size()));
  for (int r = 0; r < mRows; ++r) {
    if (r < back) {
      const std::vector<RenderCell>& line =
          mScrollback[mScrollback.size() - back + r];
      for (int c = 0; c < mCols and c < static_cast<int>(line.size()); ++c) {
        mGrid[static_cast<size_t>(r) * mCols + c] = line[c];
      }
      continue;
    }
    const int screen_row = r - back;
    for (int c = 0; c < mCols; ++c) {
      VTermScreenCell cell;
      const VTermPos pos{screen_row, c};
      if (vterm_screen_get_cell(mScreen, pos, &cell) != 0) {
        mGrid[static_cast<size_t>(r) * mCols + c] = to_render_cell(cell);
      }
    }
  }
  mGridDirty = false;
}

const std::vector<TerminalEmulator::RenderCell>& TerminalEmulator::grid() {
  if (mGridDirty) rebuild_grid();
  return mGrid;
}

void TerminalEmulator::push_scrollback(int cols, const VTermScreenCell* cells) {
  std::vector<RenderCell> line(static_cast<size_t>(mCols));
  for (int c = 0; c < mCols and c < cols; ++c) {
    line[c] = to_render_cell(cells[c]);
  }
  mScrollback.push_back(std::move(line));
  while (static_cast<int>(mScrollback.size()) > kScrollbackLimit) {
    mScrollback.pop_front();
  }
  if (mViewOffset > 0) {
    mViewOffset =
        std::min(mViewOffset + 1, static_cast<int>(mScrollback.size()));
  }
  mGridDirty = true;
}

// --- libvterm trampolines -------------------------------------------------

void TerminalEmulator::output_trampoline(const char* s, size_t len, void* user) {
  auto* self = static_cast<TerminalEmulator*>(user);
  if (self->on_pty_write and len > 0) {
    self->on_pty_write(std::string_view(s, len));
  }
}

int TerminalEmulator::damage_trampoline(VTermRect, void* user) {
  static_cast<TerminalEmulator*>(user)->mGridDirty = true;
  return 1;
}

int TerminalEmulator::movecursor_trampoline(VTermPos pos, VTermPos, int visible,
                                            void* user) {
  auto* self = static_cast<TerminalEmulator*>(user);
  self->mCursor.row = pos.row;
  self->mCursor.col = pos.col;
  self->mCursor.visible = visible != 0;
  return 1;
}

int TerminalEmulator::settermprop_trampoline(VTermProp prop, VTermValue* val,
                                             void* user) {
  auto* self = static_cast<TerminalEmulator*>(user);
  switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
      self->mCursor.visible = val->boolean != 0;
      break;
    case VTERM_PROP_CURSORBLINK:
      self->mCursor.blink = val->boolean != 0;
      break;
    case VTERM_PROP_CURSORSHAPE:
      self->mCursor.shape = val->number;
      break;
    case VTERM_PROP_ALTSCREEN:
      self->mAltScreen = val->boolean != 0;
      break;
    case VTERM_PROP_MOUSE:
      self->mMouseMode = val->number;
      break;
    case VTERM_PROP_TITLE: {
      const VTermStringFragment& frag = val->string;
      if (frag.initial) self->mTitleAccum.clear();
      self->mTitleAccum.append(frag.str, frag.len);
      if (frag.final and self->on_title) self->on_title(self->mTitleAccum);
      break;
    }
    default:
      break;
  }
  return 1;
}

int TerminalEmulator::bell_trampoline(void* user) {
  auto* self = static_cast<TerminalEmulator*>(user);
  if (self->on_bell) self->on_bell();
  return 1;
}

int TerminalEmulator::sb_pushline_trampoline(int cols,
                                             const VTermScreenCell* cells,
                                             void* user) {
  static_cast<TerminalEmulator*>(user)->push_scrollback(cols, cells);
  return 1;
}

int TerminalEmulator::sb_popline_trampoline(int cols, VTermScreenCell* cells,
                                            void* user) {
  auto* self = static_cast<TerminalEmulator*>(user);
  if (self->mScrollback.empty()) return 0;
  const std::vector<RenderCell>& line = self->mScrollback.back();
  for (int c = 0; c < cols; ++c) {
    std::memset(&cells[c], 0, sizeof(VTermScreenCell));
    cells[c].width = 1;
    if (c < static_cast<int>(line.size()) and line[c].text != " ") {
      const std::vector<std::uint32_t> cps = decode_utf8(line[c].text);
      if (not cps.empty()) cells[c].chars[0] = cps[0];
    }
  }
  self->mScrollback.pop_back();
  self->mGridDirty = true;
  return 1;
}

int TerminalEmulator::resize_trampoline(int rows, int cols, void* user) {
  auto* self = static_cast<TerminalEmulator*>(user);
  self->mRows = rows;
  self->mCols = cols;
  self->mGridDirty = true;
  return 1;
}

int TerminalEmulator::osc_trampoline(int command, VTermStringFragment frag,
                                     void* user) {
  auto* self = static_cast<TerminalEmulator*>(user);
  if (command != 7) return 0;
  if (frag.initial) self->mOscAccum.clear();
  self->mOscAccum.append(frag.str, frag.len);
  if (frag.final) {
    if (self->on_osc_cwd) self->on_osc_cwd(decode_osc7_path(self->mOscAccum));
    self->mOscAccum.clear();
  }
  return 1;
}

// --- FTXUI node ----------------------------------------------------------

namespace {

struct TerminalNode : f::Node {
  TerminalNode(TerminalEmulator& emu,
               std::function<void(int, int)> on_resize, f::Box* out_box,
               bool focused)
      : mEmu(emu),
        mOnResize(std::move(on_resize)),
        mOutBox(out_box),
        mFocused(focused) {}

  void ComputeRequirement() override {
    requirement_.min_x = 20;
    requirement_.min_y = 5;
    requirement_.flex_grow_x = 1;
    requirement_.flex_grow_y = 1;
    requirement_.flex_shrink_x = 1;
    requirement_.flex_shrink_y = 1;
  }

  void SetBox(f::Box box) override {
    f::Node::SetBox(box);
    const int w = box.x_max - box.x_min + 1;
    const int h = box.y_max - box.y_min + 1;
    if (w > 0 and h > 0 and (w != mEmu.cols() or h != mEmu.rows())) {
      if (mOnResize) mOnResize(w, h);
    }
  }

  void Render(f::Screen& screen) override {
    if (mOutBox != nullptr) *mOutBox = box_;

    const std::vector<TerminalEmulator::RenderCell>& grid = mEmu.grid();
    const int cols = mEmu.cols();
    const int rows = mEmu.rows();

    for (int r = 0; r < rows; ++r) {
      const int y = box_.y_min + r;
      if (y < box_.y_min or y > box_.y_max) break;
      if (y < screen.stencil.y_min or y > screen.stencil.y_max) continue;
      for (int c = 0; c < cols; ++c) {
        const int x = box_.x_min + c;
        if (x > box_.x_max) break;
        if (x < screen.stencil.x_min or x > screen.stencil.x_max) continue;

        const size_t idx = static_cast<size_t>(r) * cols + c;
        if (idx >= grid.size()) continue;
        const TerminalEmulator::RenderCell& rc = grid[idx];

        f::Cell& px = screen.PixelAt(x, y);
        px.character = rc.text;
        px.bold = rc.bold;
        px.dim = rc.dim;
        px.italic = rc.italic;
        px.underlined = rc.underline;
        px.blink = rc.blink;
        px.strikethrough = rc.strike;
        px.inverted = rc.reverse;
        px.foreground_color = rc.default_fg
                                  ? f::Color::Default
                                  : f::Color::RGB(rc.fr, rc.fg, rc.fb);
        px.background_color = rc.default_bg
                                  ? f::Color::Default
                                  : f::Color::RGB(rc.br, rc.bg, rc.bb);
      }
    }

    if (mFocused) {
      const TerminalEmulator::Cursor cur = mEmu.cursor();
      if (cur.visible and mEmu.view_offset() == 0) {
        f::Screen::Cursor sc;
        sc.x = box_.x_min + cur.col;
        sc.y = box_.y_min + cur.row;
        sc.shape = to_ftxui_shape(cur.shape, cur.blink);
        screen.SetCursor(sc);
      }
    }
  }

private:
  TerminalEmulator& mEmu;
  std::function<void(int, int)> mOnResize;
  f::Box* mOutBox;
  bool mFocused;
};

}  // namespace

f::Element terminal_element(TerminalEmulator& emu,
                            std::function<void(int, int)> on_resize,
                            f::Box* out_box, bool focused) {
  return std::make_shared<TerminalNode>(emu, std::move(on_resize), out_box,
                                        focused);
}

}  // namespace m8sh
