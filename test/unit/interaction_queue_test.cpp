// The question queue between background threads (MCP servers' forms, the
// agent's ask_user) and the one answer box: sessions in order and never
// interleaved, answers that only land on the question they were typed for,
// and nothing left blocked once the UI is gone.

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <common/interaction_queue.h>

namespace agentui {
namespace {

using namespace std::chrono_literals;

// Plays the UI: waits for a question to show and answers it.
std::optional<InteractionQueue::Shown> wait_for_question(InteractionQueue& queue,
                                                         uint64_t after = 0) {
  for (int i = 0; i < 400; ++i) {
    if (auto shown = queue.current(); shown and shown->id > after) return shown;
    std::this_thread::sleep_for(5ms);
  }
  return std::nullopt;
}

Question question(const std::string& message) {
  Question q;
  q.message = message;
  return q;
}

TEST(InteractionQueueTest, AnAnswerReachesTheAsker) {
  InteractionQueue queue;
  std::atomic<int> notified{0};
  queue.set_listener([&] { ++notified; });

  Answer got;
  std::thread asker([&] {
    InteractionQueue::Session session = queue.begin();
    got = session.ask(question("name?"));
  });
  const auto shown = wait_for_question(queue);
  ASSERT_TRUE(shown.has_value());
  EXPECT_EQ(shown->question.message, "name?");
  queue.answer(shown->id, {Answer::Kind::Answered, "Ada"});
  asker.join();

  EXPECT_EQ(got.kind, Answer::Kind::Answered);
  EXPECT_EQ(got.text, "Ada");
  EXPECT_FALSE(queue.current().has_value());
  EXPECT_GE(notified.load(), 2);  // shown, then gone
}

// Two servers asking at once: the second's questions wait until the first's
// session is over, so a form is never spliced with another.
TEST(InteractionQueueTest, SessionsDoNotInterleave) {
  InteractionQueue queue;
  std::mutex mutex;
  std::vector<std::string> order;

  std::thread first([&] {
    InteractionQueue::Session session = queue.begin();
    session.ask(question("a1"));
    std::this_thread::sleep_for(50ms);  // the second is waiting by now
    session.ask(question("a2"));
  });
  std::this_thread::sleep_for(20ms);  // first begins first
  std::thread second([&] {
    InteractionQueue::Session session = queue.begin();
    session.ask(question("b1"));
  });

  uint64_t last = 0;
  for (int i = 0; i < 3; ++i) {
    const auto shown = wait_for_question(queue, last);
    ASSERT_TRUE(shown.has_value());
    {
      std::lock_guard<std::mutex> lock(mutex);
      order.push_back(shown->question.message);
    }
    last = shown->id;
    queue.answer(shown->id, {Answer::Kind::Answered, "x"});
  }
  first.join();
  second.join();
  EXPECT_EQ(order, (std::vector<std::string>{"a1", "a2", "b1"}));
}

// A keystroke typed for one question must never answer the next.
TEST(InteractionQueueTest, AStaleAnswerIsIgnored) {
  InteractionQueue queue;
  Answer got;
  std::thread asker([&] {
    InteractionQueue::Session session = queue.begin();
    got = session.ask(question("q"));
  });
  const auto shown = wait_for_question(queue);
  ASSERT_TRUE(shown.has_value());
  queue.answer(shown->id + 1, {Answer::Kind::Answered, "wrong"});
  EXPECT_TRUE(queue.current().has_value());
  queue.answer(shown->id, {Answer::Kind::Declined, ""});
  // A second answer to the same question changes nothing.
  queue.answer(shown->id, {Answer::Kind::Answered, "late"});
  asker.join();
  EXPECT_EQ(got.kind, Answer::Kind::Declined);
}

TEST(InteractionQueueTest, CancelAllReleasesEveryoneNowAndLater) {
  InteractionQueue queue;
  std::vector<Answer> answers(3);
  std::thread asking([&] {
    InteractionQueue::Session session = queue.begin();
    answers[0] = session.ask(question("one"));
  });
  std::thread waiting([&] {
    std::this_thread::sleep_for(20ms);
    InteractionQueue::Session session = queue.begin();  // blocked behind `asking`
    answers[1] = session.ask(question("two"));
  });
  ASSERT_TRUE(wait_for_question(queue).has_value());
  std::this_thread::sleep_for(50ms);
  queue.cancel_all();
  asking.join();
  waiting.join();

  InteractionQueue::Session after = queue.begin();  // does not block
  answers[2] = after.ask(question("three"));
  for (const Answer& answer : answers) EXPECT_EQ(answer.kind, Answer::Kind::Cancelled);
  EXPECT_FALSE(queue.current().has_value());
}

}  // namespace
}  // namespace agentui
