# Each agent experiment lives in its own subdirectory here. Add new
# experiments by creating a folder with its own CMakeLists.txt and adding
# it below. Shared UI code lives in ../common (agentui).
add_subdirectory(chat_tui)
add_subdirectory(m8trixparrot)
add_subdirectory(toolcall)

# m8trixsh needs a terminal emulator (libvterm, via pkg-config). Only build it
# when that is available so the rest of the repo still configures without it.
if(TARGET PkgConfig::LIBVTERM)
  add_subdirectory(m8trixsh)
else()
  message(STATUS "libvterm not found (brew install libvterm) - skipping m8trixsh")
endif()
