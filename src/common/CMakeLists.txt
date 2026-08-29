# Shared UI pieces for the agent TUIs (m8trixparrot, m8trixsh): the transcript
# model and its FTXUI rendering.
add_library(agentui STATIC
  transcript_view.cpp
)

target_include_directories(agentui PUBLIC ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(agentui
  PUBLIC agentcore
  PUBLIC ftxui::dom
)
