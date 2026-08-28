#ifndef OLLAMA_CLIENT_H
#define OLLAMA_CLIENT_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <core/basic_ollama_client.h>

namespace agent {

// Singleton wrapper around BasicOllamaClient that serialises Ollama chat
// requests through a FIFO work queue. configure() must be called once before
// enqueue_chat(). Concurrent callers (e.g. subagents) each get a ticket and
// block in wait_for() until the worker thread completes their request.
struct OllamaClient {
  // Singleton access. The instance is created on first call.
  static OllamaClient& instance();

  // Set the model and (optionally) the host. Must be called before any
  // enqueue_chat() call.
  static void configure(const std::string& model,
                        const std::string& host = "http://localhost:11434");

  // Context window to request on every chat call (sent as options.num_ctx).
  // 0 leaves it to Ollama's default. Set once at startup.
  static void set_num_ctx(int64_t num_ctx);
  int64_t num_ctx() const { return mNumCtx.load(); }

  // The model's context length from /api/show (0 if it can't be determined).
  // Synchronous, runs on the caller's thread — call it before starting turns.
  int64_t context_length(std::string_view model) const;

  // Enqueues a chat request and returns a ticket number immediately.
  uint64_t enqueue_chat(const std::vector<ChatMessage>& messages,
                        const std::vector<std::string>& tools = {});

  // Blocks until the job identified by `ticket` completes. Each ticket may
  // only be waited on once.
  ChatResult wait_for(uint64_t ticket);

  // Synchronous show() — delegates to the internal BasicOllamaClient.
  ShowResult show(std::string_view model, bool verbose = false) const;

  ~OllamaClient();

private:
  OllamaClient();  // Starts the worker thread; use instance() for access.

  void worker_loop();

  BasicOllamaClient mBasicClient;
  std::string mModel;

  struct Job {
    uint64_t ticket;
    std::vector<ChatMessage> messages;
    std::vector<std::string> tools;
    std::promise<ChatResult> promise;
  };

  std::deque<Job> mQueue;
  std::mutex mQueueMutex;
  std::condition_variable mQueueCv;
  std::atomic<int64_t> mNumCtx{0};
  std::atomic<uint64_t> mNextTicket{0};
  std::unordered_map<uint64_t, std::future<ChatResult>> mResults;
  std::mutex mResultsMutex;
  std::thread mWorker;
  bool mShutdown{false};
};

}  // namespace agent

#endif  // OLLAMA_CLIENT_H
