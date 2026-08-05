#ifndef SESSION_STORE_H
#define SESSION_STORE_H

#include <string>
#include <vector>

#include <core/ollama_client.hpp>

namespace agent {

struct SessionRecord {
  std::string session_id;
  std::vector<ChatMessage> interactions;
};

struct SessionResult {
  bool ok = false;
  SessionRecord session;
  std::string error;
};

struct SessionStoreResult {
  bool ok = false;
  std::string session_id;  // The ID used — caller-supplied, or freshly generated.
  std::string error;
};

struct SessionStore {
  explicit SessionStore(std::string root_dir);

  // Writes `interactions` as a session file under root_dir. If `session_id`
  // is empty, a new UUIDv4 is generated and used. Overwrites any existing
  // file for that ID (this is a full-session checkpoint, not an append).
  SessionStoreResult store(const std::vector<ChatMessage>& interactions,
                            const std::string& session_id = "") const;

  SessionResult load(const std::string& session_id) const;

  // Finds the most recently modified session file in root_dir (by mtime)
  // and loads it, regardless of its ID.
  SessionResult latest() const;

protected:
  std::string session_file_path(const std::string& session_id) const;
  SessionResult load_from_path(const std::string& path) const;

private:
  std::string mRootDir;
};

}  // namespace agent

#endif  // SESSION_STORE_H
