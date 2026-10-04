#include <common/interaction_queue.h>

#include <utility>

namespace agentui {

InteractionQueue::Session::Session(InteractionQueue* queue, uint64_t ticket)
    : mQueue(queue), mTicket(ticket) {}

InteractionQueue::Session::Session(Session&& other) noexcept
    : mQueue(other.mQueue), mTicket(other.mTicket) {
  other.mQueue = nullptr;
}

InteractionQueue::Session::~Session() {
  if (mQueue == nullptr) return;
  {
    std::lock_guard<std::mutex> lock(mQueue->mMutex);
    if (mQueue->mServing == mTicket) ++mQueue->mServing;
  }
  mQueue->mChanged.notify_all();
}

InteractionQueue::Session InteractionQueue::begin() {
  std::unique_lock<std::mutex> lock(mMutex);
  const uint64_t ticket = mNextTicket++;
  mChanged.wait(lock, [&] { return mServing == ticket or mCancelled; });
  return Session(this, ticket);
}

Answer InteractionQueue::Session::ask(Question question) {
  InteractionQueue& queue = *mQueue;
  uint64_t id = 0;
  {
    std::lock_guard<std::mutex> lock(queue.mMutex);
    if (queue.mCancelled) return Answer{};
    id = queue.mNextId++;
    queue.mShown = Shown{id, std::move(question)};
    queue.mAnswer.reset();
  }
  queue.notify();

  Answer answer;
  {
    std::unique_lock<std::mutex> lock(queue.mMutex);
    queue.mChanged.wait(lock, [&] { return queue.mAnswer.has_value() or queue.mCancelled; });
    if (queue.mAnswer) answer = std::move(*queue.mAnswer);
    queue.mShown.reset();
    queue.mAnswer.reset();
  }
  queue.notify();
  return answer;
}

std::optional<InteractionQueue::Shown> InteractionQueue::current() const {
  std::lock_guard<std::mutex> lock(mMutex);
  // Answered but not yet collected by its asker is no longer waiting.
  if (not mShown or mAnswer or mCancelled) return std::nullopt;
  return mShown;
}

void InteractionQueue::answer(uint64_t id, Answer answer) {
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (not mShown or mShown->id != id or mAnswer) return;
    mAnswer = std::move(answer);
  }
  mChanged.notify_all();
}

void InteractionQueue::set_listener(std::function<void()> listener) {
  std::lock_guard<std::mutex> lock(mMutex);
  mListener = std::move(listener);
}

void InteractionQueue::cancel_all() {
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mCancelled = true;
  }
  mChanged.notify_all();
  notify();
}

void InteractionQueue::notify() {
  std::function<void()> listener;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    listener = mListener;
  }
  if (listener) listener();
}

}  // namespace agentui
