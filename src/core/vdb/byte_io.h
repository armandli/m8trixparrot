#ifndef M8_VDB_BYTE_IO_H
#define M8_VDB_BYTE_IO_H

#include <bit>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace vdb {

// Little-endian pack/unpack for the memory store's binary file format, shared
// by the graph snapshot (vector_index.cpp) and the record log
// (vector_store.cpp).
//
// The format is little-endian by fiat rather than by conversion: both targets
// this repo builds for are little-endian, and a byte-swapping layer would be
// untested code guarding against a host nobody runs. A big-endian host fails
// loudly here instead of silently producing a file nobody else can read.
static_assert(std::endian::native == std::endian::little,
              "the memory store's file format is little-endian only");

inline void put_u8(std::string& out, uint8_t v) {
  out.push_back(static_cast<char>(v));
}

inline void put_u16(std::string& out, uint16_t v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

inline void put_u32(std::string& out, uint32_t v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

inline void put_u64(std::string& out, uint64_t v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

inline void put_f32(std::string& out, float v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

inline void put_f64(std::string& out, double v) {
  out.append(reinterpret_cast<const char*>(&v), sizeof(v));
}

inline void put_bytes(std::string& out, const void* data, size_t len) {
  out.append(static_cast<const char*>(data), len);
}

// A cursor over a blob. Every read is bounds-checked and flips `ok` on
// overrun rather than throwing or reading past the end, so a truncated or
// corrupt record is a failed parse and not a crash.
struct ByteReader {
  explicit ByteReader(std::string_view blob) : mBlob(blob) {}

  bool ok() const { return mOk; }
  size_t offset() const { return mPos; }
  size_t remaining() const { return mOk ? mBlob.size() - mPos : 0; }

  uint8_t u8() { return read<uint8_t>(); }
  uint16_t u16() { return read<uint16_t>(); }
  uint32_t u32() { return read<uint32_t>(); }
  uint64_t u64() { return read<uint64_t>(); }
  float f32() { return read<float>(); }
  double f64() { return read<double>(); }

  std::string_view bytes(size_t len) {
    if (not mOk or mBlob.size() - mPos < len) {
      mOk = false;
      return {};
    }
    const std::string_view out = mBlob.substr(mPos, len);
    mPos += len;
    return out;
  }

  // Copies `count` floats out. Unaligned by construction — the record layout
  // packs them behind variable-length strings — so this is a memcpy, not a
  // reinterpret_cast of the cursor.
  bool floats(float* out, size_t count) {
    const std::string_view raw = bytes(count * sizeof(float));
    if (not mOk) return false;
    std::memcpy(out, raw.data(), raw.size());
    return true;
  }

protected:
  template<typename T>
  T read() {
    T value = T{};
    const std::string_view raw = bytes(sizeof(T));
    if (mOk) std::memcpy(&value, raw.data(), sizeof(T));
    return value;
  }

  std::string_view mBlob;
  size_t mPos = 0;
  bool mOk = true;
};

// CRC-32C (Castagnoli). Software table-driven: the file is written once per
// flush and read once per open, so the hardware instruction would save time
// nobody is waiting on, at the cost of an arch-specific path.
uint32_t crc32c(std::string_view data, uint32_t seed = 0);

}  // namespace vdb

#endif  // M8_VDB_BYTE_IO_H