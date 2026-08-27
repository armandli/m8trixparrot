#include <core/ollama_client.h>

namespace agent {

OllamaClient::OllamaClient() {
  mWorker = std::thread([this] { worker_loop(); });
}

OllamaClient::~OllamaClient() {
  {
    std::lock_guard<std::mutex> lk(mQueueMutex);
    mShutdown = true;
  }
  mQueueCv.notify_all();
  if (mWorker.joinable()) mWorker.join();
}

OllamaClient& OllamaClient::instance() {
  static OllamaClient inst;
  return inst;
}

void OllamaClient::configure(const std::string& model, const std::string& host) {
  OllamaClient& inst = instance();
  inst.mModel = model;
  inst.mBasicClient.mHost = host;
}

uint64_t OllamaClient::enqueue_chat(const std::vector<ChatMessage>& messages,
                                     const std::vector<std::string>& tools) {
  std::promise<ChatResult> promise;
  const uint64_t ticket = mNextTicket.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(mResultsMutex);
    mResults[ticket] = promise.get_future();
  }
  {
    std::lock_guard<std::mutex> lk(mQueueMutex);
    mQueue.push_back(Job{ticket, messages, tools, std::move(promise)});
  }
  mQueueCv.notify_one();
  return ticket;
}

ChatResult OllamaClient::wait_for(uint64_t ticket) {
  std::future<ChatResult> future;
  {
    std::lock_guard<std::mutex> lk(mResultsMutex);
    auto it = mResults.find(ticket);
    if (it == mResults.end()) {
      ChatResult err;
      err.error = "no pending request for ticket " + std::to_string(ticket);
      return err;
    }
    future = std::move(it->second);
    mResults.erase(it);
  }
  return future.get();
}

ShowResult OllamaClient::show(std::string_view model, bool verbose) const {
  return mBasicClient.show(model, verbose);
}

void OllamaClient::worker_loop() {
  while (true) {
    Job job;
    {
      std::unique_lock<std::mutex> lk(mQueueMutex);
      mQueueCv.wait(lk, [this] { return !mQueue.empty() || mShutdown; });
      if (mQueue.empty()) return;
      job = std::move(mQueue.front());
      mQueue.pop_front();
    }
    job.promise.set_value(mBasicClient.chat(mModel, job.messages, job.tools));
  }
}

}  // namespace agent
