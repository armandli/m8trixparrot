#ifndef PARALLEL_KEY_H
#define PARALLEL_KEY_H

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

namespace agent::test {

// Makes a Parallel API key visible to WebSearchTool for a live test.
// PARALLEL_API_KEY if already set; otherwise the trimmed contents of
// <repo>/.m8trix/parallel_api_key (via M8_SOURCE_DIR, since ctest's working
// directory is the build tree), exported into the environment. Returns false
// when no key is available — the caller should GTEST_SKIP.
inline bool parallel_key_available() {
  if (const char* env = std::getenv("PARALLEL_API_KEY");
      env != nullptr and *env != '\0') {
    return true;
  }

#ifdef M8_SOURCE_DIR
  std::ifstream in(std::string(M8_SOURCE_DIR) + "/.m8trix/parallel_api_key");
  if (in) {
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string key = buffer.str();
    const size_t begin = key.find_first_not_of(" \t\r\n");
    const size_t end = key.find_last_not_of(" \t\r\n");
    if (begin != std::string::npos) {
      key = key.substr(begin, end - begin + 1);
      ::setenv("PARALLEL_API_KEY", key.c_str(), 1);
      return true;
    }
  }
#endif

  return false;
}

}  // namespace agent::test

#endif  // PARALLEL_KEY_H
