// The embedded CPython interpreter (tools_python.cpp) reports sys.executable as
// THIS binary. A model script that shells out with `subprocess.run([
// sys.executable, ...])` — reaching for pip, typically — would therefore run
// the whole suite again, recursively. The sentinel env var below makes a nested
// invocation exit at once.
//
// _Exit() also skips static destruction: the interpreter is finalized only at
// process exit, and some C-extension teardown aborts when it runs after the
// module's own global state is gone. Every assertion has already been recorded
// by the time RUN_ALL_TESTS returns.

#include <cstdio>
#include <cstdlib>

#include <gtest/gtest.h>

int main(int argc, char** argv) {
  if (std::getenv("M8_INTEGRATION_TESTS_ACTIVE") != nullptr) return 0;
  ::setenv("M8_INTEGRATION_TESTS_ACTIVE", "1", 1);

  ::testing::InitGoogleTest(&argc, argv);
  const int rc = RUN_ALL_TESTS();
  std::fflush(stdout);
  std::fflush(stderr);
  std::_Exit(rc);
}
