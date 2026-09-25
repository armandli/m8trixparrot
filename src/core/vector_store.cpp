#include <core/vector_store.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#include <simdjson.h>

#include <core/byte_io.h>
#include <core/util/json_util.h>

namespace agent {

namespace {

inline constexpr char kMagic[8] = {'M', '8', 'M', 'E', 'M', 'D', 'B', '\0'};
inline constexpr uint32_t kFormatVersion = 1;
// Two of these: the header is written alternately to page 0 and page 1, so a
// torn write always leaves the other copy whole.
inline constexpr uint64_t kHeaderPage = 4096;
inline constexpr uint64_t kDataStart = 2 * kHeaderPage;
// 12 bytes of frame (length, type, padding) plus a 4-byte trailing checksum.
inline constexpr uint64_t kFrameBytes = 12;
inline constexpr uint64_t kChecksumBytes = 4;

enum struct RecordType : uint8_t {
  PutDoc = 1,
  DelDoc = 2,
  Snapshot = 3,
  // Metadata alone. recall() bumps access_count on every hit it returns, and
  // re-emitting the whole document for that would write `dim` floats per bump
  // — 3 KB a time at 768 dimensions, tens of KB per recall.
  SetMeta = 4,
};

// ─────────────────────────── metadata as JSON ──────────────────────────────
//
// The metadata encoding is JSON rather than a packed binary one: values are
// dynamically typed, the volume is small next to the vectors, and it means the
// file can be eyeballed with a hex dump when something goes wrong.

void write_meta_value(util::JsonWriter& writer, const MetaValue& value) {
  if (std::holds_alternative<std::string>(value)) {
    writer.value(std::get<std::string>(value));
  } else if (std::holds_alternative<int64_t>(value)) {
    writer.value(std::get<int64_t>(value));
  } else if (std::holds_alternative<double>(value)) {
    writer.value(std::get<double>(value));
  } else if (std::holds_alternative<bool>(value)) {
    writer.value(std::get<bool>(value));
  } else if (std::holds_alternative<std::vector<std::string>>(value)) {
    writer.value(std::get<std::vector<std::string>>(value));
  } else {
    writer.null_value();
  }
}

std::string metadata_to_json(const Metadata& meta) {
  util::JsonWriter writer;
  writer.begin_object();
  for (const auto& [name, value] : meta) {
    writer.key(name);
    write_meta_value(writer, value);
  }
  writer.end_object();
  return writer.str();
}

std::optional<MetaValue> meta_value_from(simdjson::dom::element element) {
  switch (element.type()) {
    case simdjson::dom::element_type::STRING:
      return MetaValue{std::string(element.get_string().value_unsafe())};
    case simdjson::dom::element_type::INT64:
      return MetaValue{element.get_int64().value_unsafe()};
    case simdjson::dom::element_type::UINT64:
      return MetaValue{static_cast<int64_t>(element.get_uint64().value_unsafe())};
    case simdjson::dom::element_type::DOUBLE:
      return MetaValue{element.get_double().value_unsafe()};
    case simdjson::dom::element_type::BOOL:
      return MetaValue{element.get_bool().value_unsafe()};
    case simdjson::dom::element_type::NULL_VALUE:
      return MetaValue{std::monostate{}};
    case simdjson::dom::element_type::ARRAY: {
      std::vector<std::string> items;
      for (simdjson::dom::element item : element.get_array().value_unsafe()) {
        if (item.type() != simdjson::dom::element_type::STRING) return std::nullopt;
        items.emplace_back(item.get_string().value_unsafe());
      }
      return MetaValue{std::move(items)};
    }
    default:
      return std::nullopt;
  }
}

bool metadata_from_json(std::string_view json, Metadata& out,
                        std::string& error) {
  out.clear();
  if (json.empty()) return true;
  try {
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(simdjson::padded_string(json)).get(root)) {
      error = "metadata is not valid JSON";
      return false;
    }
    simdjson::dom::object object;
    if (root.get_object().get(object)) {
      error = "metadata is not a JSON object";
      return false;
    }
    for (simdjson::dom::key_value_pair field : object) {
      const std::optional<MetaValue> value = meta_value_from(field.value);
      if (not value) {
        error = "metadata field '" + std::string(field.key) +
                "' holds an unsupported value";
        return false;
      }
      out.emplace(std::string(field.key), *value);
    }
    return true;
  } catch (const std::exception& e) {
    error = std::string("metadata parse failed: ") + e.what();
    return false;
  }
}

}  // namespace

std::string_view field_type_name(FieldType type) {
  switch (type) {
    case FieldType::String: return "string";
    case FieldType::Int: return "int";
    case FieldType::Float: return "float";
    case FieldType::Bool: return "bool";
    case FieldType::StringArray: return "string_array";
    default: return "unknown";
  }
}

namespace {

std::optional<FieldType> field_type_from_name(std::string_view name) {
  if (name == "string") return FieldType::String;
  if (name == "int") return FieldType::Int;
  if (name == "float") return FieldType::Float;
  if (name == "bool") return FieldType::Bool;
  if (name == "string_array") return FieldType::StringArray;
  return std::nullopt;
}

bool value_fits(FieldType type, const MetaValue& value) {
  if (std::holds_alternative<std::monostate>(value)) return true;
  switch (type) {
    case FieldType::String: return std::holds_alternative<std::string>(value);
    case FieldType::Int: return std::holds_alternative<int64_t>(value);
    // An int where a float is declared: JSON writes 1 for 1.0, so refusing it
    // would reject a value this store itself had written.
    case FieldType::Float:
      return std::holds_alternative<double>(value) or
             std::holds_alternative<int64_t>(value);
    case FieldType::Bool: return std::holds_alternative<bool>(value);
    case FieldType::StringArray:
      return std::holds_alternative<std::vector<std::string>>(value);
    default: return false;
  }
}

}  // namespace

// ────────────────────────────────── Schema ─────────────────────────────────

void Schema::add_field(std::string name, FieldType type) {
  for (auto& [existing, existing_type] : fields) {
    if (existing == name) {
      existing_type = type;
      return;
    }
  }
  fields.emplace_back(std::move(name), type);
}

std::optional<FieldType> Schema::field_type(std::string_view name) const {
  for (const auto& [existing, type] : fields) {
    if (existing == name) return type;
  }
  return std::nullopt;
}

bool Schema::validate(const Metadata& meta, std::string& error) const {
  for (const auto& [name, value] : meta) {
    const std::optional<FieldType> type = field_type(name);
    if (not type) {
      error = "metadata field '" + name + "' is not in the schema";
      return false;
    }
    if (not value_fits(*type, value)) {
      error = "metadata field '" + name + "' should be " +
              std::string(field_type_name(*type));
      return false;
    }
  }
  return true;
}

std::string Schema::to_json() const {
  util::JsonWriter writer;
  writer.begin_array();
  for (const auto& [name, type] : fields) {
    writer.begin_object()
        .field("name", name)
        .field("type", field_type_name(type))
        .end_object();
  }
  writer.end_array();
  return writer.str();
}

std::optional<Schema> Schema::from_json(std::string_view json,
                                        std::string& error) {
  Schema schema;
  if (json.empty()) return schema;
  try {
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(simdjson::padded_string(json)).get(root)) {
      error = "schema is not valid JSON";
      return std::nullopt;
    }
    simdjson::dom::array entries;
    if (root.get_array().get(entries)) {
      error = "schema is not a JSON array";
      return std::nullopt;
    }
    for (simdjson::dom::element entry : entries) {
      simdjson::dom::object object;
      if (entry.get_object().get(object)) {
        error = "schema entry is not an object";
        return std::nullopt;
      }
      std::string_view name;
      std::string_view type_name;
      if (object["name"].get_string().get(name) or
          object["type"].get_string().get(type_name)) {
        error = "schema entry is missing 'name' or 'type'";
        return std::nullopt;
      }
      const std::optional<FieldType> type = field_type_from_name(type_name);
      if (not type) {
        error = "schema field '" + std::string(name) + "' has unknown type '" +
                std::string(type_name) + "'";
        return std::nullopt;
      }
      schema.add_field(std::string(name), *type);
    }
    return schema;
  } catch (const std::exception& e) {
    error = std::string("schema parse failed: ") + e.what();
    return std::nullopt;
  }
}

// ────────────────────────────────── Filter ─────────────────────────────────

namespace {

std::optional<double> numeric_of(const MetaValue& value) {
  if (std::holds_alternative<int64_t>(value)) {
    return static_cast<double>(std::get<int64_t>(value));
  }
  if (std::holds_alternative<double>(value)) return std::get<double>(value);
  return std::nullopt;
}

bool values_equal(const MetaValue& a, const MetaValue& b) {
  const std::optional<double> na = numeric_of(a);
  const std::optional<double> nb = numeric_of(b);
  // Cross-type on purpose: a document that stored importance as 1 must still
  // match a filter written as 1.0, and JSON gives no say in which one arrives.
  if (na and nb) return *na == *nb;
  if (std::holds_alternative<std::string>(a) and
      std::holds_alternative<std::string>(b)) {
    return std::get<std::string>(a) == std::get<std::string>(b);
  }
  if (std::holds_alternative<bool>(a) and std::holds_alternative<bool>(b)) {
    return std::get<bool>(a) == std::get<bool>(b);
  }
  if (std::holds_alternative<std::vector<std::string>>(a) and
      std::holds_alternative<std::vector<std::string>>(b)) {
    return std::get<std::vector<std::string>>(a) ==
           std::get<std::vector<std::string>>(b);
  }
  return false;
}

// -1, 0, 1, or nothing when the two are not comparable.
std::optional<int> compare_values(const MetaValue& a, const MetaValue& b) {
  const std::optional<double> na = numeric_of(a);
  const std::optional<double> nb = numeric_of(b);
  if (na and nb) return *na < *nb ? -1 : (*na > *nb ? 1 : 0);
  if (std::holds_alternative<std::string>(a) and
      std::holds_alternative<std::string>(b)) {
    const int order = std::get<std::string>(a).compare(std::get<std::string>(b));
    return order < 0 ? -1 : (order > 0 ? 1 : 0);
  }
  return std::nullopt;
}

bool array_contains(const MetaValue& haystack, const MetaValue& needle) {
  if (not std::holds_alternative<std::vector<std::string>>(haystack) or
      not std::holds_alternative<std::string>(needle)) {
    return false;
  }
  const std::vector<std::string>& items =
      std::get<std::vector<std::string>>(haystack);
  return std::find(items.begin(), items.end(),
                   std::get<std::string>(needle)) != items.end();
}

bool in_list(const MetaValue& value, const std::vector<MetaValue>& list) {
  // A string-array field is "in" the list when any of its elements is, which is
  // how tags behave: {"tags":{"$in":["paris"]}} should find a memory tagged
  // paris and travel.
  if (std::holds_alternative<std::vector<std::string>>(value)) {
    for (const std::string& item : std::get<std::vector<std::string>>(value)) {
      for (const MetaValue& candidate : list) {
        if (values_equal(MetaValue{item}, candidate)) return true;
      }
    }
    return false;
  }
  for (const MetaValue& candidate : list) {
    if (values_equal(value, candidate)) return true;
  }
  return false;
}

bool evaluate(const FilterNode& node, const Metadata& meta) {
  if (node.op == FilterOp::And) {
    for (const FilterNode& child : node.children) {
      if (not evaluate(child, meta)) return false;
    }
    return true;
  }
  if (node.op == FilterOp::Or) {
    for (const FilterNode& child : node.children) {
      if (evaluate(child, meta)) return true;
    }
    return false;
  }

  const auto found = meta.find(node.field);
  // An absent field fails every comparison, $ne included. The alternative —
  // "absent is not equal to anything" — makes a filter silently match
  // documents written before the field existed.
  if (found == meta.end()) return false;
  const MetaValue& value = found->second;

  switch (node.op) {
    case FilterOp::Eq: return values_equal(value, node.value);
    case FilterOp::Ne: return not values_equal(value, node.value);
    case FilterOp::In: return in_list(value, node.values);
    case FilterOp::Nin: return not in_list(value, node.values);
    case FilterOp::Contains: return array_contains(value, node.value);
    default: break;
  }

  const std::optional<int> order = compare_values(value, node.value);
  if (not order) return false;
  switch (node.op) {
    case FilterOp::Lt: return *order < 0;
    case FilterOp::Lte: return *order <= 0;
    case FilterOp::Gt: return *order > 0;
    case FilterOp::Gte: return *order >= 0;
    default: return false;
  }
}

std::optional<FilterOp> comparison_op(std::string_view name) {
  if (name == "$eq") return FilterOp::Eq;
  if (name == "$ne") return FilterOp::Ne;
  if (name == "$lt") return FilterOp::Lt;
  if (name == "$lte") return FilterOp::Lte;
  if (name == "$gt") return FilterOp::Gt;
  if (name == "$gte") return FilterOp::Gte;
  if (name == "$in") return FilterOp::In;
  if (name == "$nin") return FilterOp::Nin;
  if (name == "$contains") return FilterOp::Contains;
  return std::nullopt;
}

bool parse_conditions(simdjson::dom::object object, FilterNode& into,
                      std::string& error);

bool parse_field_condition(std::string_view field,
                           simdjson::dom::element spec, FilterNode& into,
                           std::string& error) {
  if (spec.type() != simdjson::dom::element_type::OBJECT) {
    // A bare value is shorthand for $eq, matching caliby's {"field": value}.
    const std::optional<MetaValue> value = meta_value_from(spec);
    if (not value) {
      error = "filter field '" + std::string(field) +
              "' has an unsupported value";
      return false;
    }
    into.children.push_back(
        FilterNode{FilterOp::Eq, std::string(field), *value, {}, {}});
    return true;
  }

  for (simdjson::dom::key_value_pair entry : spec.get_object().value_unsafe()) {
    const std::optional<FilterOp> op = comparison_op(entry.key);
    if (not op) {
      error = "filter field '" + std::string(field) +
              "' uses unknown operator '" + std::string(entry.key) + "'";
      return false;
    }

    FilterNode leaf;
    leaf.op = *op;
    leaf.field = std::string(field);
    if (*op == FilterOp::In or *op == FilterOp::Nin) {
      simdjson::dom::array items;
      if (entry.value.get_array().get(items)) {
        error = "filter '" + std::string(entry.key) + "' on field '" +
                std::string(field) + "' needs an array";
        return false;
      }
      for (simdjson::dom::element item : items) {
        const std::optional<MetaValue> value = meta_value_from(item);
        if (not value) {
          error = "filter '" + std::string(entry.key) + "' on field '" +
                  std::string(field) + "' has an unsupported element";
          return false;
        }
        leaf.values.push_back(*value);
      }
    } else {
      const std::optional<MetaValue> value = meta_value_from(entry.value);
      if (not value) {
        error = "filter '" + std::string(entry.key) + "' on field '" +
                std::string(field) + "' has an unsupported value";
        return false;
      }
      leaf.value = *value;
    }
    into.children.push_back(std::move(leaf));
  }
  return true;
}

bool parse_conditions(simdjson::dom::object object, FilterNode& into,
                      std::string& error) {
  for (simdjson::dom::key_value_pair entry : object) {
    if (entry.key == "$and" or entry.key == "$or") {
      simdjson::dom::array branches;
      if (entry.value.get_array().get(branches)) {
        error = std::string(entry.key) + " needs an array of filters";
        return false;
      }
      FilterNode group;
      group.op = entry.key == "$and" ? FilterOp::And : FilterOp::Or;
      for (simdjson::dom::element branch : branches) {
        simdjson::dom::object branch_object;
        if (branch.get_object().get(branch_object)) {
          error = std::string(entry.key) + " holds something that is not a filter";
          return false;
        }
        FilterNode child;
        child.op = FilterOp::And;
        if (not parse_conditions(branch_object, child, error)) return false;
        group.children.push_back(std::move(child));
      }
      into.children.push_back(std::move(group));
      continue;
    }
    if (not entry.key.empty() and entry.key.front() == '$') {
      error = "unknown top-level filter operator '" + std::string(entry.key) + "'";
      return false;
    }
    if (not parse_field_condition(entry.key, entry.value, into, error)) {
      return false;
    }
  }
  return true;
}

bool is_blank(std::string_view text) {
  for (const char c : text) {
    if (not std::isspace(static_cast<unsigned char>(c))) return false;
  }
  return true;
}

}  // namespace

std::optional<Filter> Filter::parse(std::string_view json, std::string& error) {
  Filter filter;
  if (is_blank(json)) return filter;
  try {
    simdjson::dom::parser parser;
    simdjson::dom::element root;
    if (parser.parse(simdjson::padded_string(json)).get(root)) {
      error = "filter is not valid JSON";
      return std::nullopt;
    }
    simdjson::dom::object object;
    if (root.get_object().get(object)) {
      error = "filter is not a JSON object";
      return std::nullopt;
    }
    if (not parse_conditions(object, filter.root, error)) return std::nullopt;
    return filter;
  } catch (const std::exception& e) {
    error = std::string("filter parse failed: ") + e.what();
    return std::nullopt;
  }
}

bool Filter::matches(const Metadata& meta) const {
  return evaluate(root, meta);
}

// ──────────────────────────── the file format ──────────────────────────────
//
// Header page 0        4096 bytes, see encode_header
// Header page 1        4096 bytes, the previous generation
// Records              from 8192, framed as
//                        u32 payload_len | u8 type | u8 pad[3]
//                        payload | u32 crc32c(type byte + payload)
//
// The header is written alternately to page 0 and page 1, newest generation
// winning at open. A torn header write therefore always leaves a whole copy of
// the previous one, which is the difference between losing the last flush and
// losing the whole file.

namespace {

std::string encode_header(uint64_t generation, const StoreOptions& options,
                          const Schema& schema, const std::string& source,
                          uint64_t live_count, uint64_t next_id,
                          uint64_t log_end, uint64_t snapshot_offset,
                          uint64_t garbage) {
  std::string schema_json = schema.to_json();
  std::string page;
  page.reserve(kHeaderPage);
  put_bytes(page, kMagic, sizeof(kMagic));
  put_u32(page, kFormatVersion);
  put_u64(page, generation);
  put_u32(page, options.dim);
  put_u8(page, static_cast<uint8_t>(options.metric));
  put_u8(page, static_cast<uint8_t>(options.hnsw_m));
  put_u16(page, static_cast<uint16_t>(options.hnsw_ef_construction));
  put_u64(page, live_count);
  put_u64(page, next_id);
  put_u64(page, log_end);
  put_u64(page, snapshot_offset);
  put_u64(page, garbage);
  put_u32(page, static_cast<uint32_t>(schema_json.size()));
  put_u32(page, static_cast<uint32_t>(source.size()));
  page += schema_json;
  page += source;
  // The checksum covers everything ahead of it, padding included, so it also
  // catches a page whose tail was scribbled on.
  page.resize(kHeaderPage - kChecksumBytes, '\0');
  put_u32(page, crc32c(page));
  return page;
}

struct HeaderFields {
  uint64_t generation = 0;
  uint32_t dim = 0;
  Metric metric = Metric::Cosine;
  uint32_t hnsw_m = 16;
  uint32_t hnsw_ef_construction = 200;
  uint64_t live_count = 0;
  uint64_t next_id = 1;
  uint64_t log_end = kDataStart;
  uint64_t snapshot_offset = 0;
  uint64_t garbage = 0;
  std::string schema_json;
  std::string source;
};

bool decode_header(std::string_view page, HeaderFields& out) {
  if (page.size() != kHeaderPage) return false;
  const std::string_view body = page.substr(0, kHeaderPage - kChecksumBytes);
  ByteReader tail(page.substr(kHeaderPage - kChecksumBytes));
  if (tail.u32() != crc32c(body)) return false;

  ByteReader reader(body);
  if (reader.bytes(sizeof(kMagic)) !=
      std::string_view(kMagic, sizeof(kMagic))) {
    return false;
  }
  if (reader.u32() != kFormatVersion) return false;
  out.generation = reader.u64();
  out.dim = reader.u32();
  out.metric = static_cast<Metric>(reader.u8());
  out.hnsw_m = reader.u8();
  out.hnsw_ef_construction = reader.u16();
  out.live_count = reader.u64();
  out.next_id = reader.u64();
  out.log_end = reader.u64();
  out.snapshot_offset = reader.u64();
  out.garbage = reader.u64();
  const uint32_t schema_len = reader.u32();
  const uint32_t source_len = reader.u32();
  out.schema_json = std::string(reader.bytes(schema_len));
  out.source = std::string(reader.bytes(source_len));
  return reader.ok();
}

bool read_exact(int fd, uint64_t offset, char* into, size_t len) {
  size_t done = 0;
  while (done < len) {
    const ssize_t got =
        ::pread(fd, into + done, len - done, static_cast<off_t>(offset + done));
    if (got <= 0) return false;
    done += static_cast<size_t>(got);
  }
  return true;
}

bool write_exact(int fd, uint64_t offset, const char* from, size_t len) {
  size_t done = 0;
  while (done < len) {
    const ssize_t put = ::pwrite(fd, from + done, len - done,
                                 static_cast<off_t>(offset + done));
    if (put <= 0) return false;
    done += static_cast<size_t>(put);
  }
  return true;
}

bool sync_fd(int fd, bool full, std::string& error) {
#if defined(__APPLE__)
  // fsync(2) on Darwin hands the data to the drive and returns; it does not
  // wait for the drive's own cache to commit. F_FULLFSYNC is the real barrier.
  // Some filesystems refuse it (network mounts, some VMs), so a failure falls
  // through to fsync rather than failing the flush outright.
  if (full and ::fcntl(fd, F_FULLFSYNC, 0) == 0) return true;
#else
  (void)full;
#endif
  if (::fsync(fd) == 0) return true;
  error = std::string("fsync failed: ") + std::strerror(errno);
  return false;
}

// A rename is a directory metadata change: without this the old file can come
// back after a crash even though rename(2) already returned.
void sync_parent_directory(const std::string& path) {
  const std::filesystem::path parent =
      std::filesystem::path(path).parent_path();
  const std::string dir = parent.empty() ? "." : parent.string();
  const int fd = ::open(dir.c_str(), O_RDONLY);
  if (fd < 0) return;
  ::fsync(fd);
  ::close(fd);
}

std::string compact_temp_path(const std::string& path) {
  return path + ".compact";
}

std::string frame_record(uint8_t type, const std::string& payload) {
  std::string framed;
  framed.reserve(kFrameBytes + payload.size() + kChecksumBytes);
  put_u32(framed, static_cast<uint32_t>(payload.size()));
  put_u8(framed, type);
  put_u8(framed, 0);
  put_u16(framed, 0);
  // The frame's own 12 bytes are padded out so a record body starts 8-byte
  // aligned, which keeps the float array in a PutDoc payload from straddling
  // more cache lines than it has to.
  put_u32(framed, 0);
  std::string checked;
  checked.reserve(1 + payload.size());
  checked.push_back(static_cast<char>(type));
  checked += payload;
  framed += payload;
  put_u32(framed, crc32c(checked));
  return framed;
}

}  // namespace


StoreProbe probe_vector_store(const std::string& path) {
  StoreProbe probe;
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) return probe;

  std::string page(kHeaderPage, '\0');
  HeaderFields first;
  HeaderFields second;
  const bool ok_first =
      read_exact(fd, 0, page.data(), kHeaderPage) and decode_header(page, first);
  const bool ok_second = read_exact(fd, kHeaderPage, page.data(), kHeaderPage) and
                         decode_header(page, second);
  ::close(fd);
  if (not ok_first and not ok_second) return probe;

  const HeaderFields& header =
      (ok_first and (not ok_second or first.generation >= second.generation))
          ? first
          : second;
  probe.exists = true;
  probe.dim = header.dim;
  probe.metric = header.metric;
  probe.source = header.source;
  // A hint from the last header write, not a replayed count: a store opened
  // since then may hold more.
  probe.doc_count = header.live_count;
  return probe;
}

// ──────────────────────────────── VectorStore ──────────────────────────────

VectorStore::~VectorStore() {
  if (mFd < 0) return;
  if (mReady) flush();
  ::close(mFd);
  mFd = -1;
}

std::string VectorStore::put_payload_locked(const Entry& entry,
                                            const float* vector) const {
  const std::string meta = metadata_to_json(entry.metadata);
  std::string payload;
  payload.reserve(24 + entry.content.size() + meta.size() +
                  mOptions.dim * sizeof(float));
  put_u64(payload, entry.id);
  put_u32(payload, static_cast<uint32_t>(entry.content.size()));
  put_bytes(payload, entry.content.data(), entry.content.size());
  put_u32(payload, static_cast<uint32_t>(meta.size()));
  put_bytes(payload, meta.data(), meta.size());
  put_bytes(payload, vector, mOptions.dim * sizeof(float));
  return payload;
}

StoreResult VectorStore::append_record_locked(uint8_t type,
                                              const std::string& payload) {
  StoreResult result;
  const std::string framed = frame_record(type, payload);
  if (not write_exact(mFd, mLogEnd, framed.data(), framed.size())) {
    result.error = std::string("vector store: write failed: ") +
                   std::strerror(errno);
    return result;
  }
  mLogEnd += framed.size();
  result.ok = true;
  return result;
}

StoreResult VectorStore::write_header_locked() {
  StoreResult result;
  ++mGeneration;
  const std::string page =
      encode_header(mGeneration, mOptions, mSchema, mSource, mLiveCount,
                    mNextId, mLogEnd, mSnapshotOffset, mGarbage);
  const uint64_t at = (mGeneration & 1ull) * kHeaderPage;
  if (not write_exact(mFd, at, page.data(), page.size())) {
    result.error = std::string("vector store: header write failed: ") +
                   std::strerror(errno);
    return result;
  }
  result.ok = true;
  return result;
}

StoreResult VectorStore::sync_locked() {
  StoreResult result;
  std::string error;
  if (not sync_fd(mFd, mOptions.full_fsync, error)) {
    result.error = "vector store: " + error;
    return result;
  }
  result.ok = true;
  return result;
}

StoreResult VectorStore::flush_locked() {
  const std::string snapshot = mIndex->serialize_graph();
  const uint64_t previous = mSnapshotOffset;
  const uint64_t previous_end = mSnapshotEnd;
  const uint64_t at = mLogEnd;
  StoreResult appended = append_record_locked(
      static_cast<uint8_t>(RecordType::Snapshot), snapshot);
  if (not appended.ok) return appended;
  if (previous > 0) mGarbage += previous_end - previous;
  mSnapshotOffset = at;
  mSnapshotEnd = mLogEnd;

  const StoreResult header = write_header_locked();
  if (not header.ok) return header;
  return sync_locked();
}

StoreResult VectorStore::flush() {
  std::unique_lock<std::shared_mutex> lock(mMutex);
  if (mFd < 0) {
    StoreResult result;
    result.error = "vector store: not open";
    return result;
  }
  return flush_locked();
}

uint64_t VectorStore::doc_count() const {
  std::shared_lock<std::shared_mutex> lock(mMutex);
  return mLiveCount;
}

uint64_t VectorStore::garbage_bytes() const {
  std::shared_lock<std::shared_mutex> lock(mMutex);
  return mGarbage;
}

uint64_t VectorStore::file_size() const {
  std::shared_lock<std::shared_mutex> lock(mMutex);
  return mLogEnd;
}

AddResult VectorStore::add(const std::vector<std::string>& contents,
                           const std::vector<Metadata>& metadatas,
                           const std::vector<std::vector<float>>& vectors) {
  AddResult result;
  if (contents.size() != metadatas.size() or
      contents.size() != vectors.size()) {
    result.error = "vector store: contents, metadatas and vectors must be the "
                   "same length";
    return result;
  }

  std::unique_lock<std::shared_mutex> lock(mMutex);
  if (mFd < 0) {
    result.error = "vector store: not open";
    return result;
  }

  // Everything is validated before anything is written: a batch that fails
  // half way through would leave the file holding documents the caller was
  // told were rejected.
  for (size_t i = 0; i < contents.size(); ++i) {
    if (vectors[i].size() != mOptions.dim) {
      result.error = "vector store: vector " + std::to_string(i) + " has " +
                     std::to_string(vectors[i].size()) +
                     " dimensions, expected " + std::to_string(mOptions.dim);
      return result;
    }
    std::string schema_error;
    if (not mSchema.validate(metadatas[i], schema_error)) {
      result.error = "vector store: document " + std::to_string(i) + ": " +
                     schema_error;
      return result;
    }
  }

  result.ids.reserve(contents.size());
  for (size_t i = 0; i < contents.size(); ++i) {
    Entry entry;
    entry.id = mNextId;
    entry.content = contents[i];
    entry.metadata = metadatas[i];

    const std::string payload = put_payload_locked(entry, vectors[i].data());
    const uint64_t before = mLogEnd;
    const StoreResult appended = append_record_locked(
        static_cast<uint8_t>(RecordType::PutDoc), payload);
    if (not appended.ok) {
      result.error = appended.error;
      return result;
    }
    entry.record_bytes = mLogEnd - before;

    const uint32_t label = mIndex->add(vectors[i].data());
    if (label >= mEntries.size()) mEntries.resize(label + 1);
    mEntries[label] = std::move(entry);
    mIdToLabel[mNextId] = label;
    result.ids.push_back(mNextId);
    ++mNextId;
    ++mLiveCount;
  }

  result.ok = true;
  return result;
}

StoreResult VectorStore::update_metadata(uint64_t id, const Metadata& changes) {
  StoreResult result;
  std::unique_lock<std::shared_mutex> lock(mMutex);
  if (mFd < 0) {
    result.error = "vector store: not open";
    return result;
  }
  const auto found = mIdToLabel.find(id);
  if (found == mIdToLabel.end() or mEntries[found->second].deleted) {
    result.error = "vector store: no document with id " + std::to_string(id);
    return result;
  }

  Entry& entry = mEntries[found->second];
  Metadata merged = entry.metadata;
  for (const auto& [name, value] : changes) merged[name] = value;
  std::string schema_error;
  if (not mSchema.validate(merged, schema_error)) {
    result.error = "vector store: " + schema_error;
    return result;
  }

  // The whole merged map goes on disk, not a delta, so replay is a plain
  // assignment and the order of overlapping SetMeta records cannot matter.
  const std::string meta = metadata_to_json(merged);
  std::string payload;
  put_u64(payload, id);
  put_u32(payload, static_cast<uint32_t>(meta.size()));
  put_bytes(payload, meta.data(), meta.size());

  const uint64_t before = mLogEnd;
  const StoreResult appended = append_record_locked(
      static_cast<uint8_t>(RecordType::SetMeta), payload);
  if (not appended.ok) return appended;

  mGarbage += entry.meta_bytes;
  entry.meta_bytes = mLogEnd - before;
  entry.metadata = std::move(merged);
  result.ok = true;
  return result;
}

StoreResult VectorStore::remove(const std::vector<uint64_t>& ids) {
  StoreResult result;
  std::unique_lock<std::shared_mutex> lock(mMutex);
  if (mFd < 0) {
    result.error = "vector store: not open";
    return result;
  }
  for (const uint64_t id : ids) {
    const auto found = mIdToLabel.find(id);
    if (found == mIdToLabel.end()) continue;
    Entry& entry = mEntries[found->second];
    if (entry.deleted) continue;

    std::string payload;
    put_u64(payload, id);
    const uint64_t before = mLogEnd;
    const StoreResult appended = append_record_locked(
        static_cast<uint8_t>(RecordType::DelDoc), payload);
    if (not appended.ok) return appended;

    entry.deleted = true;
    // The document's own record, its metadata updates, and the tombstone
    // itself are all dead weight now; only a compaction reclaims them.
    mGarbage += entry.record_bytes + entry.meta_bytes + (mLogEnd - before);
    if (mLiveCount > 0) --mLiveCount;
  }
  result.ok = true;
  return result;
}

std::vector<Document> VectorStore::get(
    const std::vector<uint64_t>& ids) const {
  std::shared_lock<std::shared_mutex> lock(mMutex);
  std::vector<Document> out;
  out.reserve(ids.size());
  for (const uint64_t id : ids) {
    const auto found = mIdToLabel.find(id);
    if (found == mIdToLabel.end()) continue;
    const Entry& entry = mEntries[found->second];
    if (entry.deleted) continue;
    out.push_back(Document{entry.id, entry.content, entry.metadata});
  }
  return out;
}

std::vector<Document> VectorStore::get_where(const Filter& filter,
                                             size_t limit) const {
  std::shared_lock<std::shared_mutex> lock(mMutex);
  std::vector<Document> out;
  for (const Entry& entry : mEntries) {
    if (out.size() >= limit) break;
    if (entry.deleted) continue;
    if (not filter.is_match_all() and not filter.matches(entry.metadata)) {
      continue;
    }
    out.push_back(Document{entry.id, entry.content, entry.metadata});
  }
  return out;
}

SearchResult VectorStore::search(const std::vector<float>& query, size_t k,
                                 const Filter& filter) const {
  std::shared_lock<std::shared_mutex> lock(mMutex);
  return search_locked(query, k, filter);
}

SearchResult VectorStore::search_locked(const std::vector<float>& query,
                                        size_t k,
                                        const Filter& filter) const {
  SearchResult result;
  if (mFd < 0) {
    result.error = "vector store: not open";
    return result;
  }
  if (query.size() != mOptions.dim) {
    result.error = "vector store: query has " + std::to_string(query.size()) +
                   " dimensions, expected " + std::to_string(mOptions.dim);
    return result;
  }
  if (k == 0 or mLiveCount == 0) {
    result.ok = true;
    return result;
  }

  std::vector<float> normalized = query;
  if (mOptions.metric == Metric::Cosine) {
    normalize(normalized.data(), normalized.size());
  }

  const bool match_all = filter.is_match_all();
  const LabelPredicate accept = [&](uint32_t label) {
    const Entry& entry = mEntries[label];
    if (entry.deleted) return false;
    return match_all or filter.matches(entry.metadata);
  };

  // Small collections take the exact path: at a few thousand documents a
  // sequential NEON scan beats a graph walk of random 3 KB gathers, and it is
  // exact rather than approximate.
  const bool exact = mLiveCount <= mOptions.brute_force_below;
  std::vector<Neighbor> hits =
      exact ? mIndex->scan(normalized.data(), k, accept)
            : mIndex->search(normalized.data(), k, accept);
  // A selective filter starves the graph walk: the beam fills with documents
  // the filter rejects and fewer than k survive. Falling back to the exact
  // scan turns the threshold above into a performance knob rather than a
  // correctness one.
  if (not exact and hits.size() < k) {
    hits = mIndex->scan(normalized.data(), k, accept);
  }

  result.hits.reserve(hits.size());
  for (const Neighbor& hit : hits) {
    const Entry& entry = mEntries[hit.label];
    ScoredDoc scored;
    scored.id = entry.id;
    scored.score = mOptions.metric == Metric::Cosine ? 1.0f - hit.distance
                                                     : -hit.distance;
    scored.document = Document{entry.id, entry.content, entry.metadata};
    result.hits.push_back(std::move(scored));
  }
  result.ok = true;
  return result;
}

StoreOpenResult VectorStore::open(const std::string& path, const Schema& schema,
                                  const StoreOptions& options) {
  StoreOpenResult result;
  if (options.dim == 0) {
    result.error = "vector store: options.dim must be set";
    return result;
  }

  std::error_code ec;
  const std::filesystem::path parent = std::filesystem::path(path).parent_path();
  if (not parent.empty()) {
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      result.error = "vector store: cannot create " + parent.string() + ": " +
                     ec.message();
      return result;
    }
  }
  // An orphan temp file is debris from a crash during compaction. The real
  // file was never touched, so the orphan is simply dropped.
  std::filesystem::remove(compact_temp_path(path), ec);

  std::unique_ptr<VectorStore> store(new VectorStore());
  store->mPath = path;
  store->mOptions = options;
  store->mSchema = schema;
  store->mSource = options.source;

  // A POSIX descriptor rather than an fstream: F_FULLFSYNC, pwrite and
  // ftruncate all need one.
  store->mFd = ::open(path.c_str(), O_RDWR | O_CREAT, 0600);
  if (store->mFd < 0) {
    result.error = "vector store: cannot open " + path + ": " +
                   std::strerror(errno);
    return result;
  }

  const uintmax_t on_disk = std::filesystem::file_size(path, ec);
  const uint64_t file_bytes = ec ? 0 : static_cast<uint64_t>(on_disk);

  IndexParams index_params;
  index_params.dim = options.dim;
  index_params.metric = options.metric;
  index_params.m = options.hnsw_m;
  index_params.ef_construction = options.hnsw_ef_construction;
  index_params.ef_search = options.ef_search;

  if (file_bytes < kDataStart) {
    store->mIndex = std::make_unique<VectorIndex>(index_params);
    store->mLogEnd = kDataStart;
    if (::ftruncate(store->mFd, static_cast<off_t>(kDataStart)) != 0) {
      result.error = std::string("vector store: cannot size the file: ") +
                     std::strerror(errno);
      return result;
    }
    // Twice, so both header pages are valid from the start and the ping-pong
    // never has to read a page that was never written.
    for (int i = 0; i < 2; ++i) {
      const StoreResult written = store->write_header_locked();
      if (not written.ok) {
        result.error = written.error;
        return result;
      }
    }
    const StoreResult synced = store->sync_locked();
    if (not synced.ok) {
      result.error = synced.error;
      return result;
    }
    sync_parent_directory(path);
    store->mReady = true;
    result.ok = true;
    result.store = std::move(store);
    return result;
  }

  std::string page0(kHeaderPage, '\0');
  std::string page1(kHeaderPage, '\0');
  HeaderFields first;
  HeaderFields second;
  const bool ok_first =
      read_exact(store->mFd, 0, page0.data(), kHeaderPage) and
      decode_header(page0, first);
  const bool ok_second =
      read_exact(store->mFd, kHeaderPage, page1.data(), kHeaderPage) and
      decode_header(page1, second);
  if (not ok_first and not ok_second) {
    result.error = "vector store: " + path +
                   " is not a vector store file, or both of its headers are "
                   "corrupt";
    return result;
  }
  const HeaderFields& header =
      (ok_first and (not ok_second or first.generation >= second.generation))
          ? first
          : second;

  if (header.dim != options.dim) {
    result.error = "vector store: " + path + " holds " +
                   std::to_string(header.dim) +
                   "-dimensional vectors, but " + std::to_string(options.dim) +
                   " was requested";
    return result;
  }
  if (header.metric != options.metric) {
    result.error = "vector store: " + path +
                   " was written with a different distance metric";
    return result;
  }
  if (not options.source.empty() and not header.source.empty() and
      options.source != header.source) {
    result.warning = "vector store: " + path + " was written by '" +
                     header.source + "' but '" + options.source +
                     "' is configured now; stored vectors are not comparable "
                     "with new ones";
  }

  store->mGeneration = header.generation;
  store->mSource = header.source;
  index_params.m = header.hnsw_m;
  index_params.ef_construction = header.hnsw_ef_construction;
  store->mIndex = std::make_unique<VectorIndex>(index_params);

  std::string schema_error;
  // The file's own schema wins: it describes the bytes that are actually
  // there, where the caller's argument only describes what it expected.
  const std::optional<Schema> stored =
      Schema::from_json(header.schema_json, schema_error);
  if (stored) store->mSchema = *stored;

  // ── replay ──
  //
  // The scan runs to end of file, not to the header's log_end: records
  // appended since the last header write are on disk and intact, and the
  // checksum is what separates them from a torn tail. That is what lets an
  // append skip the header write entirely.
  uint64_t offset = kDataStart;
  uint64_t valid_end = kDataStart;
  uint64_t snapshot_at = 0;
  uint64_t snapshot_end = 0;
  std::string snapshot_payload;
  std::string frame(kFrameBytes, '\0');

  while (offset + kFrameBytes + kChecksumBytes <= file_bytes) {
    if (not read_exact(store->mFd, offset, frame.data(), kFrameBytes)) break;
    ByteReader head(frame);
    const uint32_t payload_len = head.u32();
    const uint8_t type = head.u8();
    const uint64_t total = kFrameBytes + payload_len + kChecksumBytes;
    // Checked before anything is allocated, so a corrupt length can never turn
    // into a huge read.
    if (offset + total > file_bytes) break;

    std::string body(payload_len + kChecksumBytes, '\0');
    if (not read_exact(store->mFd, offset + kFrameBytes, body.data(),
                       body.size())) {
      break;
    }
    const std::string_view payload(body.data(), payload_len);
    ByteReader trailer(std::string_view(body).substr(payload_len));
    std::string checked;
    checked.reserve(1 + payload_len);
    checked.push_back(static_cast<char>(type));
    checked.append(payload);
    if (trailer.u32() != crc32c(checked)) break;

    bool applied = true;
    switch (static_cast<RecordType>(type)) {
      case RecordType::PutDoc: {
        ByteReader reader(payload);
        Entry entry;
        entry.id = reader.u64();
        entry.content = std::string(reader.bytes(reader.u32()));
        const std::string meta_json = std::string(reader.bytes(reader.u32()));
        std::vector<float> vector(options.dim);
        reader.floats(vector.data(), options.dim);
        if (not reader.ok()) {
          applied = false;
          break;
        }
        std::string meta_error;
        metadata_from_json(meta_json, entry.metadata, meta_error);
        entry.record_bytes = total;

        // Every PutDoc loads a vector, superseded ones included, so a label is
        // always its record's position in the log. That is the invariant the
        // graph snapshot depends on: it stores no ids, only dense labels.
        const uint32_t label = store->mIndex->load_vector(vector.data());
        if (label >= store->mEntries.size()) {
          store->mEntries.resize(label + 1);
        }
        const auto previous = store->mIdToLabel.find(entry.id);
        if (previous != store->mIdToLabel.end()) {
          Entry& older = store->mEntries[previous->second];
          if (not older.deleted) {
            older.deleted = true;
            store->mGarbage += older.record_bytes + older.meta_bytes;
            if (store->mLiveCount > 0) --store->mLiveCount;
          }
        }
        store->mEntries[label] = std::move(entry);
        store->mIdToLabel[store->mEntries[label].id] = label;
        ++store->mLiveCount;
        store->mNextId =
            std::max(store->mNextId, store->mEntries[label].id + 1);
        break;
      }
      case RecordType::SetMeta: {
        ByteReader reader(payload);
        const uint64_t id = reader.u64();
        const std::string meta_json = std::string(reader.bytes(reader.u32()));
        if (not reader.ok()) {
          applied = false;
          break;
        }
        const auto found = store->mIdToLabel.find(id);
        if (found != store->mIdToLabel.end()) {
          Entry& entry = store->mEntries[found->second];
          store->mGarbage += entry.meta_bytes;
          entry.meta_bytes = total;
          std::string meta_error;
          metadata_from_json(meta_json, entry.metadata, meta_error);
        } else {
          store->mGarbage += total;
        }
        break;
      }
      case RecordType::DelDoc: {
        ByteReader reader(payload);
        const uint64_t id = reader.u64();
        if (not reader.ok()) {
          applied = false;
          break;
        }
        const auto found = store->mIdToLabel.find(id);
        if (found != store->mIdToLabel.end()) {
          Entry& entry = store->mEntries[found->second];
          if (not entry.deleted) {
            entry.deleted = true;
            store->mGarbage += entry.record_bytes + entry.meta_bytes;
            if (store->mLiveCount > 0) --store->mLiveCount;
          }
        }
        store->mGarbage += total;
        break;
      }
      case RecordType::Snapshot: {
        if (snapshot_at > 0) store->mGarbage += snapshot_end - snapshot_at;
        snapshot_at = offset;
        snapshot_end = offset + total;
        snapshot_payload = std::string(payload);
        break;
      }
      default:
        applied = false;
        break;
    }
    if (not applied) break;

    offset += total;
    valid_end = offset;
  }

  if (valid_end < file_bytes) {
    result.truncated_bytes = file_bytes - valid_end;
    result.warning = "vector store: discarded " +
                     std::to_string(result.truncated_bytes) +
                     " bytes of a torn tail from " + path;
    if (::ftruncate(store->mFd, static_cast<off_t>(valid_end)) != 0) {
      result.error = std::string("vector store: cannot truncate: ") +
                     std::strerror(errno);
      return result;
    }
  }
  store->mLogEnd = valid_end;
  store->mSnapshotOffset = snapshot_at;
  store->mSnapshotEnd = snapshot_end;

  std::string graph_error;
  if (snapshot_payload.empty() or
      not store->mIndex->deserialize_graph(snapshot_payload, graph_error)) {
    // No usable snapshot: rebuild the whole graph. Correct, just slower, so a
    // damaged snapshot costs time rather than data.
    if (not snapshot_payload.empty()) {
      store->mGarbage += snapshot_end - snapshot_at;
      store->mSnapshotOffset = 0;
      store->mSnapshotEnd = 0;
    }
    // The vectors stay: only the graph is dropped, so the link loop below
    // rebuilds it over everything that was replayed.
    store->mIndex->reset_graph();
  }
  for (uint32_t label = store->mIndex->linked_count();
       label < store->mIndex->size(); ++label) {
    store->mIndex->link(label);
  }

  if (options.compact_on_open and store->mGarbage > 0 and
      store->mGarbage > store->mLogEnd - store->mGarbage) {
    const StoreResult compacted = store->compact_locked();
    if (not compacted.ok) {
      result.error = compacted.error;
      return result;
    }
  }

  store->mReady = true;
  result.ok = true;
  result.store = std::move(store);
  return result;
}

StoreResult VectorStore::compact() {
  std::unique_lock<std::shared_mutex> lock(mMutex);
  return compact_locked();
}

StoreResult VectorStore::compact_locked() {
  StoreResult result;
  if (mFd < 0) {
    result.error = "vector store: not open";
    return result;
  }

  IndexParams index_params = mIndex->params();
  auto rebuilt = std::make_unique<VectorIndex>(index_params);
  std::vector<Entry> kept;
  kept.reserve(mLiveCount);
  std::string body;

  for (uint32_t label = 0; label < mEntries.size(); ++label) {
    const Entry& entry = mEntries[label];
    if (entry.deleted) continue;
    const float* vector = mIndex->vector_at(label);
    // The graph is rebuilt rather than remapped: after many deletes the
    // surviving adjacency has been pruned down by every re-link, so remapping
    // would carry that degraded connectivity forward. At this store's scale a
    // rebuild is milliseconds.
    rebuilt->add(vector);
    Entry copy = entry;
    const std::string payload = put_payload_locked(copy, vector);
    const std::string framed =
        frame_record(static_cast<uint8_t>(RecordType::PutDoc), payload);
    copy.record_bytes = framed.size();
    copy.meta_bytes = 0;
    body += framed;
    kept.push_back(std::move(copy));
  }

  const uint64_t snapshot_at = kDataStart + body.size();
  const std::string snapshot = frame_record(
      static_cast<uint8_t>(RecordType::Snapshot), rebuilt->serialize_graph());
  body += snapshot;

  const std::string temp = compact_temp_path(mPath);
  const int fd = ::open(temp.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    result.error = "vector store: cannot create " + temp + ": " +
                   std::strerror(errno);
    return result;
  }

  bool wrote = ::ftruncate(fd, static_cast<off_t>(kDataStart)) == 0 and
               write_exact(fd, kDataStart, body.data(), body.size());
  const uint64_t new_end = kDataStart + body.size();
  for (uint64_t generation = 1; wrote and generation <= 2; ++generation) {
    const std::string page =
        encode_header(generation, mOptions, mSchema, mSource,
                      static_cast<uint64_t>(kept.size()), mNextId, new_end,
                      snapshot_at, 0);
    wrote = write_exact(fd, (generation & 1ull) * kHeaderPage, page.data(),
                        page.size());
  }
  std::string sync_error;
  if (not wrote or not sync_fd(fd, mOptions.full_fsync, sync_error)) {
    ::close(fd);
    std::error_code ignored;
    std::filesystem::remove(temp, ignored);
    result.error = "vector store: compaction write failed" +
                   (sync_error.empty() ? std::string() : ": " + sync_error);
    return result;
  }
  ::close(fd);

  // The original is untouched until this one call, so a crash anywhere above
  // leaves the database exactly as it was and only the temp file orphaned.
  std::error_code ec;
  std::filesystem::rename(temp, mPath, ec);
  if (ec) {
    std::filesystem::remove(temp, ec);
    result.error = "vector store: cannot replace " + mPath + ": " +
                   ec.message();
    return result;
  }
  sync_parent_directory(mPath);

  ::close(mFd);
  mFd = ::open(mPath.c_str(), O_RDWR, 0600);
  if (mFd < 0) {
    result.error = "vector store: cannot reopen " + mPath + " after "
                   "compaction: " + std::strerror(errno);
    return result;
  }

  mIndex = std::move(rebuilt);
  mEntries = std::move(kept);
  mIdToLabel.clear();
  for (uint32_t label = 0; label < mEntries.size(); ++label) {
    mIdToLabel[mEntries[label].id] = label;
  }
  mLiveCount = mEntries.size();
  mLogEnd = new_end;
  mSnapshotOffset = snapshot_at;
  mSnapshotEnd = new_end;
  mGarbage = 0;
  mGeneration = 2;
  result.ok = true;
  return result;
}

}  // namespace agent
