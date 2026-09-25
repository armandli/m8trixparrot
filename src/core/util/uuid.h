#ifndef M8_UTIL_UUID_H
#define M8_UTIL_UUID_H

#include <string>

namespace util {

// RFC 4122 version-4 (random) UUID. Thread-safe via thread_local RNG.
//
// Lives here rather than beside the session store because it has nothing to do
// with sessions: the agent pool names its nodes with it, and BashReplSession
// builds its end-of-command marker from it. Keeping it in session_store.h made
// the tools component depend on the agent component for one helper.
std::string generate_uuid_v4();

}  // namespace util

#endif  // M8_UTIL_UUID_H
