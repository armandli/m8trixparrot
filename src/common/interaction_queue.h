#ifndef M8_COMMON_INTERACTION_QUEUE_H
#define M8_COMMON_INTERACTION_QUEUE_H

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

// Questions from background threads — an MCP server's form, the agent's
// ask_user — for a UI that shows one at a time. No UI code in here, so it is
// tested on its own and any front end can host it.
namespace agentui {

struct Question {
  std::string heading;   // who asks
  std::string message;
  std::string detail;    // may run to several lines
  std::string emphasis;  // a part of `detail` to highlight
  std::string problem;   // why the last answer was not taken
  std::string progress;  // "field 2 of 3"
  std::string initial;   // what the answer box starts with
  std::string keys;      // how to answer
};

struct Answer {
  enum struct Kind : uint8_t { Answered, Declined, Cancelled };
  Kind kind = Kind::Cancelled;
  std::string text;
};

// A FIFO of question sessions. A session holds the queue from its first
// question to its end, so two servers' forms never interleave; sessions are
// served in the order they began.
class InteractionQueue {
public:
  class Session {
  public:
    Session(Session&& other) noexcept;
    Session& operator=(Session&&) = delete;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    // Ends the session: the next one's questions may show.
    ~Session();

    // Shows `question` and blocks until the person answers it, or the queue
    // is cancelled (then: Cancelled).
    Answer ask(Question question);

  private:
    friend class InteractionQueue;
    Session(InteractionQueue* queue, uint64_t ticket);

    InteractionQueue* mQueue;
    uint64_t mTicket;
  };

  // Blocks until every session begun earlier has ended (or cancel_all()).
  Session begin();

  // ── the UI's side ──
  struct Shown {
    uint64_t id = 0;  // new for every question
    Question question;
  };
  // The question waiting for an answer, if any.
  std::optional<Shown> current() const;
  // Answers question `id`. An id that is no longer the one shown is ignored,
  // so a keystroke meant for one question can never answer the next.
  void answer(uint64_t id, Answer answer);
  // Called — from whichever thread, never under the queue's lock — whenever
  // current() changes; a UI wakes its loop here.
  void set_listener(std::function<void()> listener);
  // Cancels the question shown and every session, now and later. For
  // shutdown: nothing may stay blocked on a person who has left.
  void cancel_all();

private:
  void notify();

  mutable std::mutex mMutex;
  std::condition_variable mChanged;
  uint64_t mNextTicket = 0;
  uint64_t mServing = 0;
  bool mCancelled = false;
  uint64_t mNextId = 1;
  std::optional<Shown> mShown;
  std::optional<Answer> mAnswer;  // for mShown
  std::function<void()> mListener;
};

}  // namespace agentui

#endif  // M8_COMMON_INTERACTION_QUEUE_H
