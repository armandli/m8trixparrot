#include <core/tools_util.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

#include <git2.h>
#include <simdjson.h>

namespace agent {

namespace {

// git_libgit2_init() is refcounted, so calling it from more than one
// translation unit is fine; the function-local static keeps it to once here.
void ensure_git2_initialized() {
  static const int init_result = git_libgit2_init();
  (void)init_result;
}

std::string trim_trailing_slash(std::string path) {
  if (not path.empty() and path.back() == '/') {
    path.pop_back();
  }
  return path;
}

// A name unique enough that two concurrent tool calls don't collide, without
// pulling in a random source: the clock plus a per-process counter.
std::string temp_file_name(std::string_view label) {
  static int counter = 0;
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  const auto ticks =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();

  std::ostringstream name;
  name << "m8trixparrot-" << label << "-" << ticks << "-" << counter++ << ".txt";
  return name.str();
}

// The offset to cut `text` at: the end of the max_lines'th line, or the end of
// the last line that fits in max_bytes, whichever comes first. text.size() when
// the whole thing fits.
size_t cut_offset(const std::string& text, size_t max_lines, size_t max_bytes) {
  size_t lines = 0;
  size_t line_start = 0;

  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '\n') continue;

    const size_t line_end = i + 1;
    if (line_end > max_bytes) return line_start;

    ++lines;
    if (lines >= max_lines) return line_end;
    line_start = line_end;
  }

  // Trailing text with no final newline.
  if (text.size() > max_bytes) return line_start;
  return text.size();
}

// Reads a JSON array into whichever ToolArgValue alternative fits its
// elements: strings become a vector<string>, {oldText,newText} objects become
// the pair vector (that's `edit`'s `edits`). An array of anything else has no
// alternative and is skipped by returning nullopt.
std::optional<ToolArgValue> array_arg(simdjson::ondemand::array array) {
  std::vector<std::string> strings;
  std::vector<std::pair<std::string, std::string>> pairs;

  for (auto element : array) {
    simdjson::ondemand::value value;
    if (element.get(value)) continue;

    simdjson::ondemand::json_type type;
    if (value.type().get(type)) continue;

    if (type == simdjson::ondemand::json_type::string) {
      std::string_view item;
      if (value.get_string().get(item)) continue;
      strings.emplace_back(item);
    } else if (type == simdjson::ondemand::json_type::object) {
      simdjson::ondemand::object item;
      if (value.get_object().get(item)) continue;
      // Read both keys in one pass — ondemand can't revisit a field.
      std::string old_text;
      std::string new_text;
      for (auto field : item) {
        std::string_view key;
        if (field.unescaped_key().get(key)) continue;
        std::string_view text;
        if (field.value().get_string().get(text)) continue;
        if (key == "oldText") {
          old_text = text;
        } else if (key == "newText") {
          new_text = text;
        }
      }
      pairs.emplace_back(std::move(old_text), std::move(new_text));
    }
  }

  if (not pairs.empty()) return ToolArgValue(std::move(pairs));
  if (not strings.empty()) return ToolArgValue(std::move(strings));
  return std::nullopt;
}

std::optional<ToolArgValue> scalar_arg(simdjson::ondemand::value value,
                                       simdjson::ondemand::json_type type) {
  switch (type) {
    case simdjson::ondemand::json_type::string: {
      std::string_view text;
      if (value.get_string().get(text)) return std::nullopt;
      return ToolArgValue(std::string(text));
    }
    case simdjson::ondemand::json_type::boolean: {
      bool flag = false;
      if (value.get_bool().get(flag)) return std::nullopt;
      return ToolArgValue(flag);
    }
    case simdjson::ondemand::json_type::number: {
      simdjson::ondemand::number number;
      if (value.get_number().get(number)) return std::nullopt;
      if (number.is_double()) return ToolArgValue(number.get_double());
      if (number.is_int64()) return ToolArgValue(number.get_int64());
      return ToolArgValue(static_cast<int64_t>(number.get_uint64()));
    }
    default:
      return std::nullopt;
  }
}

}  // namespace

// ---------------------------------------------------------------------------

std::optional<std::string> string_arg(const ToolArgs& args,
                                      std::string_view name) {
  const auto it = args.find(name);
  if (it == args.end()) return std::nullopt;
  if (const auto* value = std::get_if<std::string>(&it->second)) return *value;
  return std::nullopt;
}

std::optional<int64_t> int_arg(const ToolArgs& args, std::string_view name) {
  const auto it = args.find(name);
  if (it == args.end()) return std::nullopt;
  if (const auto* value = std::get_if<int64_t>(&it->second)) return *value;
  if (const auto* value = std::get_if<double>(&it->second)) {
    return static_cast<int64_t>(*value);
  }
  return std::nullopt;
}

std::optional<bool> bool_arg(const ToolArgs& args, std::string_view name) {
  const auto it = args.find(name);
  if (it == args.end()) return std::nullopt;
  if (const auto* value = std::get_if<bool>(&it->second)) return *value;
  return std::nullopt;
}

const std::vector<std::pair<std::string, std::string>>* pairs_arg(
    const ToolArgs& args, std::string_view name) {
  const auto it = args.find(name);
  if (it == args.end()) return nullptr;
  return std::get_if<std::vector<std::pair<std::string, std::string>>>(
      &it->second);
}

ToolArgs args_from_json(std::string_view json, std::string& error) {
  ToolArgs args;

  simdjson::ondemand::parser parser;
  simdjson::padded_string padded(json);

  simdjson::ondemand::document document;
  if (parser.iterate(padded).get(document)) {
    error = "tool arguments are not valid JSON";
    return args;
  }

  simdjson::ondemand::object object;
  if (document.get_object().get(object)) {
    error = "tool arguments are not a JSON object";
    return args;
  }

  for (auto field : object) {
    std::string_view key;
    if (field.unescaped_key().get(key)) continue;

    simdjson::ondemand::value value;
    if (field.value().get(value)) continue;

    simdjson::ondemand::json_type type;
    if (value.type().get(type)) continue;

    std::optional<ToolArgValue> parsed;
    if (type == simdjson::ondemand::json_type::array) {
      simdjson::ondemand::array array;
      if (not value.get_array().get(array)) {
        parsed = array_arg(array);
      }
    } else {
      parsed = scalar_arg(value, type);
    }

    if (parsed) args.emplace(std::string(key), std::move(*parsed));
  }

  return args;
}

// ---------------------------------------------------------------------------

TruncatedOutput truncate_output(std::string text, std::string_view label,
                                size_t max_lines, size_t max_bytes) {
  TruncatedOutput output;

  const size_t cut = cut_offset(text, max_lines, max_bytes);
  if (cut >= text.size()) {
    output.text = std::move(text);
    return output;
  }

  output.truncated = true;

  std::error_code ec;
  const std::filesystem::path path =
      std::filesystem::temp_directory_path(ec) / temp_file_name(label);
  if (not ec) {
    std::ofstream out(path, std::ios::binary);
    if (out) {
      out.write(text.data(), static_cast<std::streamsize>(text.size()));
      if (out.good()) output.overflow_path = path.string();
    }
  }

  text.resize(cut);
  output.text = std::move(text);
  return output;
}

std::string truncation_note(const TruncatedOutput& output) {
  if (not output.truncated) return std::string();
  if (output.overflow_path.empty()) {
    return "\n[output truncated; the full output could not be saved]";
  }
  return "\n[output truncated; full output saved to " + output.overflow_path +
         "]";
}

// ---------------------------------------------------------------------------

void IgnoreFilter::RepoDeleter::operator()(git_repository* repo) const {
  git_repository_free(repo);
}

IgnoreFilter::IgnoreFilter(const std::string& start_path) {
  ensure_git2_initialized();

  git_repository* repo = nullptr;
  if (git_repository_open_ext(&repo, start_path.c_str(), 0, nullptr) != 0) {
    return;
  }
  mRepo.reset(repo);

  const char* workdir = git_repository_workdir(mRepo.get());
  if (workdir != nullptr) mWorkdir = trim_trailing_slash(workdir);
}

IgnoreFilter::~IgnoreFilter() = default;

bool IgnoreFilter::ignored(const std::string& path, bool is_directory) const {
  std::error_code ec;
  const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
  const std::filesystem::path& target = ec ? std::filesystem::path(path) : absolute;

  // .git is never worth walking into, repository or not.
  for (const auto& part : target) {
    if (part == ".git") return true;
  }

  if (not mRepo or mWorkdir.empty()) return false;

  // git_ignore_path_is_ignored() wants a path relative to the work tree; a
  // path outside it has no ignore rules to match.
  const std::filesystem::path relative =
      std::filesystem::relative(target, mWorkdir, ec);
  if (ec or relative.empty() or *relative.begin() == "..") return false;

  std::string candidate = relative.generic_string();
  if (is_directory) candidate += "/";

  int is_ignored = 0;
  if (git_ignore_path_is_ignored(&is_ignored, mRepo.get(), candidate.c_str()) !=
      0) {
    return false;
  }
  return is_ignored == 1;
}

// ---------------------------------------------------------------------------

bool is_binary(std::string_view head) {
  return head.find('\0') != std::string_view::npos;
}

std::optional<std::string> read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (not in) return std::nullopt;

  std::ostringstream buffer;
  buffer << in.rdbuf();
  return buffer.str();
}

}  // namespace agent
