#include <core/mcp/schema.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <optional>
#include <set>

#include <core/util/base64.h>
#include <core/util/text.h>

namespace mcp {

namespace {

using util::JsonValue;

// A description longer than this is cut: it is re-sent on every model call for
// as long as the tool is loaded.
constexpr size_t kMaxDescription = 2048;

bool is_type_name(std::string_view name) {
  return name == "string" or name == "number" or name == "integer" or
         name == "boolean" or name == "object" or name == "array" or
         name == "null";
}

bool is_scalar(const JsonValue& value) {
  return value.is_string() or value.is_number() or value.is_bool() or
         value.is_null();
}

// Cuts at a UTF-8 character boundary so a clipped description stays valid.
std::string clip(std::string text, size_t limit) {
  if (text.size() <= limit) return text;
  size_t cut = limit;
  while (cut > 0 and (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) {
    --cut;
  }
  text.resize(cut);
  return text + "...";
}

std::string compact(const JsonValue& value, size_t limit = 60) {
  return clip(value.dump(), limit);
}

std::string number_text(const JsonValue& value) {
  if (value.is_int()) return std::to_string(value.as_int());
  std::string text = value.dump();
  return text;
}

// The constraints Ollama would drop, as words the model can still read.
std::vector<std::string> constraint_notes(const JsonValue& node) {
  std::vector<std::string> notes;
  if (const JsonValue* value = node.find("default")) {
    notes.push_back("default " + compact(*value));
  }
  if (const std::string& format = node.get("format").as_string();
      not format.empty()) {
    notes.push_back("format " + format);
  }
  const JsonValue& minimum = node.get("minimum");
  const JsonValue& maximum = node.get("maximum");
  if (minimum.is_number()) notes.push_back("min " + number_text(minimum));
  if (maximum.is_number()) notes.push_back("max " + number_text(maximum));
  if (node.get("exclusiveMinimum").is_number()) {
    notes.push_back("> " + number_text(node.get("exclusiveMinimum")));
  }
  if (node.get("exclusiveMaximum").is_number()) {
    notes.push_back("< " + number_text(node.get("exclusiveMaximum")));
  }
  if (node.get("minLength").is_number() or node.get("maxLength").is_number()) {
    notes.push_back("length " +
                    (node.get("minLength").is_number()
                         ? number_text(node.get("minLength"))
                         : std::string("0")) +
                    ".." +
                    (node.get("maxLength").is_number()
                         ? number_text(node.get("maxLength"))
                         : std::string("")));
  }
  if (node.get("minItems").is_number() or node.get("maxItems").is_number()) {
    notes.push_back("items " +
                    (node.get("minItems").is_number()
                         ? number_text(node.get("minItems"))
                         : std::string("0")) +
                    ".." +
                    (node.get("maxItems").is_number()
                         ? number_text(node.get("maxItems"))
                         : std::string("")));
  }
  if (const std::string& pattern = node.get("pattern").as_string();
      not pattern.empty()) {
    notes.push_back("pattern " + clip(pattern, 80));
  }
  if (node.get("deprecated").as_bool(false)) notes.push_back("deprecated");
  return notes;
}

std::string join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += sep;
    out += parts[i];
  }
  return out;
}

// The `type` of a lowered node as a list, whichever way it was spelled.
std::vector<std::string> types_of(const JsonValue& node) {
  std::vector<std::string> out;
  const JsonValue& type = node.get("type");
  if (type.is_string()) out.push_back(type.as_string());
  for (const JsonValue& item : type.items()) {
    if (item.is_string()) out.push_back(item.as_string());
  }
  return out;
}

void set_types(JsonValue& node, std::vector<std::string> types) {
  std::vector<std::string> unique;
  for (std::string& type : types) {
    if (std::find(unique.begin(), unique.end(), type) == unique.end()) {
      unique.push_back(std::move(type));
    }
  }
  if (unique.empty()) {
    node.erase("type");
  } else if (unique.size() == 1) {
    node.set("type", unique.front());
  } else {
    JsonValue array = JsonValue::array();
    for (const std::string& type : unique) array.push_back(type);
    node.set("type", std::move(array));
  }
}

// A lowered alternative that is only a type (and maybe a description) can be
// folded into a type union instead of an anyOf.
bool is_type_only(const JsonValue& node) {
  for (const JsonValue::Member& member : node.members()) {
    if (member.key != "type" and member.key != "description") return false;
  }
  return node.contains("type");
}

struct Lowerer {
  const JsonValue& root;
  std::vector<std::string>& notes;
  int nodes = 0;

  static constexpr int kMaxNodes = 4000;
  static constexpr int kMaxDepth = 16;
  static constexpr int kMaxRefDepth = 8;

  // A JSON pointer into the root document ("#", "#/$defs/x", "#/definitions/x"
  // or any other local path). Never a network fetch: the spec forbids
  // dereferencing remote `$ref`s by default, and nothing here needs them.
  const JsonValue* resolve(std::string_view ref) const {
    if (ref.empty() or ref.front() != '#') return nullptr;
    ref.remove_prefix(1);
    const JsonValue* node = &root;
    while (not ref.empty()) {
      if (ref.front() != '/') return nullptr;
      ref.remove_prefix(1);
      const size_t slash = ref.find('/');
      std::string segment(ref.substr(0, slash));
      ref = slash == std::string_view::npos ? std::string_view()
                                            : ref.substr(slash);
      // JSON pointer escapes: ~1 is '/', ~0 is '~' (in that order).
      for (size_t at = segment.find("~1"); at != std::string::npos;
           at = segment.find("~1", at + 1)) {
        segment.replace(at, 2, "/");
      }
      for (size_t at = segment.find("~0"); at != std::string::npos;
           at = segment.find("~0", at + 1)) {
        segment.replace(at, 2, "~");
      }
      if (node->is_object()) {
        node = node->find(segment);
      } else if (node->is_array()) {
        char* end = nullptr;
        const long index = std::strtol(segment.c_str(), &end, 10);
        if (end == segment.c_str() or *end != '\0' or index < 0 or
            static_cast<size_t>(index) >= node->items().size()) {
          return nullptr;
        }
        node = &node->items()[static_cast<size_t>(index)];
      } else {
        return nullptr;
      }
      if (node == nullptr) return nullptr;
    }
    return node;
  }

  // allOf: properties and required unite; the first type wins.
  static void merge_into(JsonValue& out, const JsonValue& part) {
    for (const JsonValue::Member& member : part.members()) {
      if (member.key == "properties") {
        JsonValue& properties = out["properties"];
        for (const JsonValue::Member& property : member.value.members()) {
          properties.set(property.key, property.value);
        }
      } else if (member.key == "required") {
        JsonValue& required = out["required"];
        for (const JsonValue& name : member.value.items()) {
          bool present = false;
          for (const JsonValue& existing : required.items()) {
            present = present or existing == name;
          }
          if (not present) required.push_back(name);
        }
      } else if (member.key == "description") {
        const std::string& existing = out.get("description").as_string();
        if (existing.empty()) {
          out.set("description", member.value);
        } else if (existing.find(member.value.as_string()) ==
                   std::string::npos) {
          out.set("description", existing + " " + member.value.as_string());
        }
      } else if (not out.contains(member.key)) {
        out.set(member.key, member.value);
      }
    }
  }

  // nullopt means "matches nothing" (a `false` schema): the property goes.
  std::optional<JsonValue> lower(const JsonValue& node, int depth,
                                 int ref_depth) {
    if (++nodes > kMaxNodes or depth > kMaxDepth) {
      if (nodes == kMaxNodes + 1) notes.push_back("schema truncated (too large)");
      JsonValue out = JsonValue::object();
      out.set("description", "(nested schema omitted)");
      return out;
    }
    if (node.is_bool()) {
      if (node.as_bool()) return JsonValue::object();
      return std::nullopt;
    }
    if (not node.is_object()) return JsonValue::object();

    JsonValue out = JsonValue::object();
    std::vector<std::string> extra_notes;

    // $ref first: sibling keywords then refine what it pointed at, which is
    // the 2020-12 reading of `$ref` next to other keywords.
    if (const std::string& ref = node.get("$ref").as_string(); not ref.empty()) {
      const JsonValue* target = resolve(ref);
      if (target != nullptr and ref_depth < kMaxRefDepth) {
        std::optional<JsonValue> base = lower(*target, depth + 1, ref_depth + 1);
        if (not base) return std::nullopt;
        out = std::move(*base);
      } else {
        notes.push_back("unresolved $ref " + clip(ref, 80));
        extra_notes.push_back("schema at " + clip(ref, 60));
      }
    }

    if (const JsonValue& all = node.get("allOf"); all.is_array()) {
      for (const JsonValue& part : all.items()) {
        std::optional<JsonValue> lowered = lower(part, depth + 1, ref_depth);
        if (not lowered) return std::nullopt;
        merge_into(out, *lowered);
      }
    }

    // anyOf and oneOf say the same thing to a model; Ollama only keeps anyOf.
    std::vector<JsonValue> alternatives;
    for (const char* key : {"anyOf", "oneOf"}) {
      for (const JsonValue& option : node.get(key).items()) {
        std::optional<JsonValue> lowered = lower(option, depth + 1, ref_depth);
        if (lowered) alternatives.push_back(std::move(*lowered));
      }
    }
    if (not alternatives.empty()) {
      // `X | null` is how JSON Schema spells "optional": keep X.
      std::vector<JsonValue> real;
      for (JsonValue& option : alternatives) {
        const std::vector<std::string> types = types_of(option);
        if (types.size() == 1 and types.front() == "null" and
            option.size() == 1) {
          continue;
        }
        real.push_back(std::move(option));
      }
      if (real.size() == 1) {
        merge_into(out, real.front());
      } else if (not real.empty() and
                 std::all_of(real.begin(), real.end(), is_type_only) and
                 not out.contains("type")) {
        std::vector<std::string> types;
        for (const JsonValue& option : real) {
          for (std::string& type : types_of(option)) types.push_back(type);
        }
        set_types(out, std::move(types));
      } else if (not real.empty()) {
        JsonValue any = JsonValue::array();
        for (JsonValue& option : real) {
          if (option.size() == 0) continue;  // `true`: anything goes
          any.push_back(std::move(option));
        }
        if (any.size() > 0) out.set("anyOf", std::move(any));
      }
    }

    const JsonValue& type = node.get("type");
    if (type.is_string() or type.is_array()) {
      std::vector<std::string> types;
      for (const std::string& name : types_of(node)) {
        if (is_type_name(name)) types.push_back(name);
      }
      // A nullable type is an optional one to the model.
      if (types.size() > 1) {
        types.erase(std::remove(types.begin(), types.end(), "null"),
                    types.end());
      }
      if (not types.empty()) set_types(out, std::move(types));
    }

    if (const JsonValue& values = node.get("enum"); values.is_array()) {
      JsonValue kept = JsonValue::array();
      for (const JsonValue& value : values.items()) {
        if (is_scalar(value) and kept.size() < 200) kept.push_back(value);
      }
      if (kept.size() > 0) out.set("enum", std::move(kept));
    } else if (const JsonValue* constant = node.find("const");
               constant != nullptr and is_scalar(*constant)) {
      JsonValue kept = JsonValue::array();
      kept.push_back(*constant);
      out.set("enum", std::move(kept));
    }

    const JsonValue* items = node.find("items");
    if (items != nullptr and items->is_array()) {
      items = items->items().empty() ? nullptr : &items->items().front();
    }
    if (items == nullptr) {
      const JsonValue& prefix = node.get("prefixItems");
      if (not prefix.items().empty()) items = &prefix.items().front();
    }
    if (items != nullptr) {
      std::optional<JsonValue> lowered = lower(*items, depth + 1, ref_depth);
      if (lowered) out.set("items", std::move(*lowered));
    }

    if (const JsonValue& properties = node.get("properties");
        properties.is_object()) {
      JsonValue& lowered_properties = out["properties"];
      for (const JsonValue::Member& property : properties.members()) {
        std::optional<JsonValue> lowered =
            lower(property.value, depth + 1, ref_depth);
        if (lowered) lowered_properties.set(property.key, std::move(*lowered));
      }
    }

    if (const JsonValue& required = node.get("required"); required.is_array()) {
      JsonValue& merged = out["required"];
      for (const JsonValue& name : required.items()) {
        if (not name.is_string()) continue;
        bool present = false;
        for (const JsonValue& existing : merged.items()) {
          present = present or existing == name;
        }
        if (not present) merged.push_back(name);
      }
    }

    // Ollama keeps `required` only as strings naming real properties.
    if (JsonValue* required = out.find("required")) {
      const JsonValue& properties = out.get("properties");
      JsonValue kept = JsonValue::array();
      for (const JsonValue& name : required->items()) {
        if (name.is_string() and properties.contains(name.as_string())) {
          kept.push_back(name);
        }
      }
      if (kept.size() == 0) {
        out.erase("required");
      } else {
        *required = std::move(kept);
      }
    }

    if (out.contains("properties") and not out.contains("type") and
        not out.contains("anyOf")) {
      out.set("type", "object");
    }

    // Free-form objects say so, since additionalProperties will not survive.
    if (not node.contains("properties") and
        (node.get("additionalProperties").is_object() or
         node.get("additionalProperties").as_bool(false))) {
      extra_notes.push_back("any keys");
    }

    std::string description = node.get("description").as_string();
    if (description.empty()) description = out.get("description").as_string();
    if (description.empty()) description = node.get("title").as_string();
    std::vector<std::string> all_notes = constraint_notes(node);
    all_notes.insert(all_notes.end(), extra_notes.begin(), extra_notes.end());
    if (not all_notes.empty()) {
      if (not description.empty()) description += " ";
      description += "(" + join(all_notes, "; ") + ")";
    }
    if (not description.empty()) {
      out.set("description",
              clip(util::sanitize_utf8(description), kMaxDescription));
    }
    return out;
  }
};

std::string property_problem(const JsonValue& property, const std::string& at);

std::string properties_problem(const JsonValue& properties,
                               const std::string& at) {
  if (properties.is_null()) return std::string();
  if (not properties.is_object()) return at + ".properties is not an object";
  for (const JsonValue::Member& member : properties.members()) {
    std::string problem = property_problem(member.value, at + "." + member.key);
    if (not problem.empty()) return problem;
  }
  return std::string();
}

std::string required_problem(const JsonValue& required, const std::string& at) {
  if (required.is_null()) return std::string();
  if (not required.is_array()) return at + ".required is not an array";
  for (const JsonValue& name : required.items()) {
    if (not name.is_string()) return at + ".required holds a non-string";
  }
  return std::string();
}

// Mirrors ollama's api.ToolProperty unmarshalling.
std::string property_problem(const JsonValue& property, const std::string& at) {
  if (not property.is_object()) return at + " is not an object";
  const JsonValue& type = property.get("type");
  if (not type.is_null() and not type.is_string()) {
    if (not type.is_array()) return at + ".type is neither a string nor an array";
    for (const JsonValue& name : type.items()) {
      if (not name.is_string()) return at + ".type holds a non-string";
    }
  }
  const JsonValue& description = property.get("description");
  if (not description.is_null() and not description.is_string()) {
    return at + ".description is not a string";
  }
  const JsonValue& values = property.get("enum");
  if (not values.is_null() and not values.is_array()) {
    return at + ".enum is not an array";
  }
  if (std::string problem = required_problem(property.get("required"), at);
      not problem.empty()) {
    return problem;
  }
  if (std::string problem = properties_problem(property.get("properties"), at);
      not problem.empty()) {
    return problem;
  }
  const JsonValue& any = property.get("anyOf");
  if (not any.is_null()) {
    if (not any.is_array()) return at + ".anyOf is not an array";
    for (size_t i = 0; i < any.items().size(); ++i) {
      std::string problem = property_problem(
          any.items()[i], at + ".anyOf[" + std::to_string(i) + "]");
      if (not problem.empty()) return problem;
    }
  }
  return std::string();
}

std::string type_label(const JsonValue& schema) {
  if (const JsonValue& values = schema.get("enum"); values.size() > 0) {
    std::string out;
    const size_t shown = std::min<size_t>(values.size(), 4);
    for (size_t i = 0; i < shown; ++i) {
      if (i > 0) out += "|";
      out += compact(values.items()[i], 24);
    }
    if (values.size() > shown) out += "|...";
    return out;
  }
  if (const JsonValue& any = schema.get("anyOf"); any.size() > 0) {
    std::string out;
    for (size_t i = 0; i < any.items().size() and i < 4; ++i) {
      if (i > 0) out += "|";
      out += type_label(any.items()[i]);
    }
    return out;
  }
  const std::vector<std::string> types = types_of(schema);
  if (types.empty()) return "any";
  std::string out;
  for (size_t i = 0; i < types.size(); ++i) {
    if (i > 0) out += "|";
    if (types[i] == "array") {
      const JsonValue& items = schema.get("items");
      out += (items.is_object() ? type_label(items) : std::string("any")) + "[]";
    } else {
      out += types[i];
    }
  }
  return out;
}

bool wants(const std::vector<std::string>& types, std::string_view type) {
  return std::find(types.begin(), types.end(), type) != types.end();
}

std::optional<double> parse_number(const std::string& text) {
  if (text.empty()) return std::nullopt;
  char* end = nullptr;
  const double value = std::strtod(text.c_str(), &end);
  if (end == text.c_str() or *end != '\0' or not std::isfinite(value)) {
    return std::nullopt;
  }
  return value;
}

std::string lower_ascii(std::string text) {
  for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return text;
}

JsonValue fix_value(const JsonValue& value, const JsonValue& schema,
                    const std::string& path, std::vector<std::string>* fixes) {
  const std::vector<std::string> types = types_of(schema);
  const auto note = [&](const std::string& what) {
    if (fixes != nullptr) fixes->push_back(path + ": " + what);
  };

  if (value.is_string() and not types.empty() and not wants(types, "string")) {
    const std::string& text = value.as_string();
    if (wants(types, "integer") or wants(types, "number")) {
      if (std::optional<double> number = parse_number(text)) {
        note("string to number");
        if (wants(types, "integer") and *number == std::floor(*number) and
            std::fabs(*number) < 9.0e15) {
          return JsonValue(static_cast<int64_t>(*number));
        }
        return JsonValue(*number);
      }
    }
    if (wants(types, "boolean")) {
      const std::string lowered = lower_ascii(text);
      if (lowered == "true" or lowered == "false") {
        note("string to boolean");
        return JsonValue(lowered == "true");
      }
    }
    if (wants(types, "object") or wants(types, "array")) {
      size_t start = 0;
      while (start < text.size() and std::isspace(static_cast<unsigned char>(text[start]))) {
        ++start;
      }
      if (start < text.size() and (text[start] == '{' or text[start] == '[')) {
        if (std::optional<JsonValue> parsed = JsonValue::parse(text)) {
          if ((parsed->is_object() and wants(types, "object")) or
              (parsed->is_array() and wants(types, "array"))) {
            note("decoded JSON text");
            return fix_value(*parsed, schema, path, fixes);
          }
        }
      }
    }
    return value;
  }

  if ((value.is_number() or value.is_bool()) and wants(types, "string") and
      not wants(types, "number") and not wants(types, "integer") and
      not wants(types, "boolean")) {
    note("to string");
    if (value.is_bool()) return JsonValue(value.as_bool() ? "true" : "false");
    return JsonValue(number_text(value));
  }

  if (value.is_object() and schema.get("properties").is_object()) {
    JsonValue out = value;
    for (JsonValue::Member& member : out.members()) {
      const JsonValue* property = schema.get("properties").find(member.key);
      if (property == nullptr) continue;
      member.value = fix_value(member.value, *property, path + "." + member.key,
                               fixes);
    }
    return out;
  }

  if (value.is_array() and schema.get("items").is_object()) {
    JsonValue out = value;
    for (size_t i = 0; i < out.items().size(); ++i) {
      out.items()[i] = fix_value(out.items()[i], schema.get("items"),
                                 path + "[" + std::to_string(i) + "]", fixes);
    }
    return out;
  }
  return value;
}

// RFC 9110 tchar.
bool is_token(std::string_view text) {
  if (text.empty()) return false;
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) continue;
    if (std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos) {
      continue;
    }
    return false;
  }
  return true;
}

size_t count_annotations(const JsonValue& node) {
  size_t count = 0;
  if (node.is_object()) {
    for (const JsonValue::Member& member : node.members()) {
      if (member.key == "x-mcp-header") ++count;
      count += count_annotations(member.value);
    }
  } else if (node.is_array()) {
    for (const JsonValue& item : node.items()) count += count_annotations(item);
  }
  return count;
}

}  // namespace

LoweredSchema lower_schema(const util::JsonValue& input_schema) {
  LoweredSchema lowered;
  Lowerer lowerer{input_schema, lowered.notes};

  std::optional<JsonValue> body = lowerer.lower(input_schema, 0, 0);
  if (not body) {
    lowered.notes.push_back("root schema is `false`");
    body = JsonValue::object();
  }

  // A root of alternatives (oneOf: [{...}, {...}]) has no single property
  // list. Offer the union, requiring only what every alternative requires.
  if (not body->contains("properties") and body->get("anyOf").size() > 0) {
    JsonValue properties = JsonValue::object();
    std::optional<std::set<std::string>> common;
    for (const JsonValue& option : body->get("anyOf").items()) {
      for (const JsonValue::Member& property :
           option.get("properties").members()) {
        if (not properties.contains(property.key)) {
          properties.set(property.key, property.value);
        }
      }
      std::set<std::string> names;
      for (const JsonValue& name : option.get("required").items()) {
        names.insert(name.as_string());
      }
      if (not common) {
        common = names;
      } else {
        std::set<std::string> both;
        std::set_intersection(common->begin(), common->end(), names.begin(),
                              names.end(), std::inserter(both, both.begin()));
        common = both;
      }
    }
    body->set("properties", std::move(properties));
    if (common and not common->empty()) {
      JsonValue required = JsonValue::array();
      for (const std::string& name : *common) required.push_back(name);
      body->set("required", std::move(required));
    }
    lowered.notes.push_back("root alternatives merged");
  }

  JsonValue parameters = JsonValue::object();
  parameters.set("type", "object");
  const JsonValue& properties = body->get("properties");
  parameters.set("properties",
                 properties.is_object() ? properties : JsonValue::object());
  if (const JsonValue& required = body->get("required"); required.size() > 0) {
    parameters.set("required", required);
  }
  lowered.parameters = std::move(parameters);
  return lowered;
}

std::string ollama_shape_problem(const util::JsonValue& parameters) {
  if (not parameters.is_object()) return "parameters is not an object";
  if (not parameters.get("type").is_string()) {
    return "parameters.type is not a string";
  }
  if (std::string problem =
          properties_problem(parameters.get("properties"), "parameters");
      not problem.empty()) {
    return problem;
  }
  return required_problem(parameters.get("required"), "parameters");
}

std::string tool_schema_json(std::string_view name, std::string_view description,
                             const util::JsonValue& parameters) {
  JsonValue schema = JsonValue::object();
  schema.set("name", name);
  schema.set("description",
             clip(util::sanitize_utf8(description), kMaxDescription));
  schema.set("parameters", parameters);
  return schema.dump();
}

std::string tool_signature(std::string_view name,
                           const util::JsonValue& parameters, size_t limit) {
  std::set<std::string> required;
  for (const JsonValue& item : parameters.get("required").items()) {
    required.insert(item.as_string());
  }
  std::string out(name);
  out += "(";
  bool first = true;
  for (const JsonValue::Member& property :
       parameters.get("properties").members()) {
    if (not first) out += ", ";
    first = false;
    out += property.key;
    if (required.count(property.key) == 0) out += "?";
    out += ": " + type_label(property.value);
  }
  out += ")";
  if (out.size() > limit) out = clip(out, limit - 4) + ")";
  return out;
}

util::JsonValue arguments_object(std::string_view raw, std::string& error) {
  size_t start = 0;
  while (start < raw.size() and std::isspace(static_cast<unsigned char>(raw[start]))) {
    ++start;
  }
  if (start == raw.size()) return JsonValue::object();

  std::string parse_error;
  std::optional<JsonValue> parsed = JsonValue::parse(raw, &parse_error);
  if (not parsed) {
    error = "arguments are not valid JSON (" + parse_error + ")";
    return JsonValue::object();
  }
  if (parsed->is_null()) return JsonValue::object();
  if (parsed->is_string()) {
    // The whole object, JSON-encoded a second time.
    std::optional<JsonValue> inner = JsonValue::parse(parsed->as_string());
    if (inner and inner->is_object()) return *inner;
  }
  if (not parsed->is_object()) {
    error = "arguments must be a JSON object";
    return JsonValue::object();
  }
  return *parsed;
}

util::JsonValue fix_arguments(const util::JsonValue& arguments,
                              const util::JsonValue& parameters,
                              std::vector<std::string>* fixes) {
  if (not arguments.is_object()) return arguments;
  return fix_value(arguments, parameters, "arguments", fixes);
}

std::string collect_header_params(const util::JsonValue& input_schema,
                                  std::vector<HeaderParam>& out) {
  out.clear();
  const size_t total = count_annotations(input_schema);
  if (total == 0) return std::string();

  std::set<std::string> seen;
  std::string problem;
  size_t reachable = 0;

  const std::function<void(const JsonValue&, std::vector<std::string>&)> walk =
      [&](const JsonValue& node, std::vector<std::string>& path) {
        for (const JsonValue::Member& property :
             node.get("properties").members()) {
          if (not problem.empty()) return;
          path.push_back(property.key);
          if (const JsonValue* header = property.value.find("x-mcp-header")) {
            ++reachable;
            const std::string& name = header->as_string();
            const std::string& type = property.value.get("type").as_string();
            if (not is_token(name)) {
              problem = "x-mcp-header on '" + property.key +
                        "' is not a valid header name";
            } else if (not seen.insert(lower_ascii(name)).second) {
              problem = "x-mcp-header '" + name + "' is used twice";
            } else if (type != "string" and type != "integer" and
                       type != "boolean") {
              problem = "x-mcp-header on '" + property.key +
                        "' is not on a string, integer or boolean";
            } else {
              out.push_back(HeaderParam{path, name});
            }
          }
          walk(property.value, path);
          path.pop_back();
        }
      };

  std::vector<std::string> path;
  walk(input_schema, path);
  if (problem.empty() and reachable != total) {
    problem = "x-mcp-header on a property not reachable through properties";
  }
  if (not problem.empty()) out.clear();
  return problem;
}

std::vector<std::pair<std::string, std::string>> header_values(
    const std::vector<HeaderParam>& params, const util::JsonValue& arguments) {
  std::vector<std::pair<std::string, std::string>> out;
  constexpr int64_t kSafeInteger = 9007199254740991;  // 2^53 - 1
  for (const HeaderParam& param : params) {
    const JsonValue* value = &arguments;
    for (const std::string& key : param.path) {
      value = value->find(key);
      if (value == nullptr) break;
    }
    if (value == nullptr or value->is_null()) continue;

    std::string text;
    if (value->is_string()) {
      text = value->as_string();
    } else if (value->is_bool()) {
      text = value->as_bool() ? "true" : "false";
    } else if (value->is_number()) {
      const int64_t number = value->as_int(INT64_MIN);
      if (number == INT64_MIN or number > kSafeInteger or
          number < -kSafeInteger) {
        continue;  // not an integer the header can carry; the server decides
      }
      text = std::to_string(number);
    } else {
      continue;
    }
    out.emplace_back("Mcp-Param-" + param.name, encode_header_value(text));
  }
  return out;
}

std::string encode_header_value(std::string_view value) {
  bool plain = not value.empty();
  for (const char c : value) {
    const auto u = static_cast<unsigned char>(c);
    if (not(u == '\t' or (u >= 0x20 and u <= 0x7E))) {
      plain = false;
      break;
    }
  }
  if (plain) {
    const char first = value.front();
    const char last = value.back();
    if (first == ' ' or first == '\t' or last == ' ' or last == '\t') {
      plain = false;
    }
  }
  // A literal value that looks like the sentinel must be encoded too, or the
  // server would decode it.
  if (plain and value.rfind("=?base64?", 0) == 0 and value.size() >= 11 and
      value.substr(value.size() - 2) == "?=") {
    plain = false;
  }
  if (value.empty()) return std::string();
  if (plain) return std::string(value);
  return "=?base64?" + util::base64_encode(value) + "?=";
}

}  // namespace mcp
