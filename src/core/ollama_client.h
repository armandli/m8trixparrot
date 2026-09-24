#ifndef OLLAMA_CLIENT_H
#define OLLAMA_CLIENT_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <core/basic_ollama_client.h>

namespace agent {

// How many requests may be in flight against Ollama at once, chat and embed
// together. Ollama is one server on one machine: the point of this whole class
// is that this number is the only thing that decides how hard it is pushed.
inline constexpr int kDefaultOllamaJobs = 2;

// Singleton work pool in front of Ollama. Every chat and every embedding in the
// process goes through here, so the total number of concurrent requests is
// capped at set_concurrency() (default kDefaultOllamaJobs) no matter how many
// agent threads are running. Callers enqueue, get a ticket back immediately,
// and block in wait_for()/wait_for_embed() until a worker finishes their job.
//
// Two queues, one pool. Embeddings are drained ahead of chats because they are
// three orders of magnitude shorter: a recall that took 50ms of model time
// should not sit behind a multi-minute chat just because it was enqueued after
// it. Chat cannot starve, because every embedding is enqueued by a caller that
// is already blocked waiting for it — the number outstanding is bounded by the
// number of live agents, and each one clears in milliseconds.
//
// configure() must be called before the first enqueue_chat(), and
// configure_embed() before the first enqueue_embed() that does not name a
// model explicitly.
struct OllamaClient {
  // Singleton access. The instance is created on first call.
  static OllamaClient& instance();

  // Set the chat model and (optionally) the host. Must be called before any
  // enqueue_chat() call.
  static void configure(const std::string& model,
                        const std::string& host = "http://localhost:11434");

  // Set the default embedding model, the one enqueue_embed() uses when it is
  // not given one. An empty `host` leaves the configured host in force, so an
  // app that has already called configure() can pass the model alone.
  static void configure_embed(const std::string& embed_model,
                              const std::string& host = "");

  // Cap on requests in flight against Ollama. Honoured only while the worker
  // pool has not started yet — the pool spins up on the first enqueue, and a
  // call after that is ignored, so apps set this during startup.
  static void set_concurrency(int jobs);

  // The cap actually in force. Once the pool is running this is its size,
  // because set_concurrency() stops taking effect at that point.
  int concurrency() const { return mConcurrency.load(); }

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

  // Blocks until the chat job identified by `ticket` completes. Each ticket may
  // only be waited on once.
  ChatResult wait_for(uint64_t ticket);

  // Enqueues an embedding request and returns a ticket number immediately. An
  // empty `model` means the one configure_embed() set.
  uint64_t enqueue_embed(const std::vector<std::string>& input,
                         std::string_view model = {});

  // Blocks until the embed job identified by `ticket` completes. Each ticket
  // may only be waited on once. Tickets are drawn from one counter shared with
  // enqueue_chat(), so passing a chat ticket here is reported as unknown rather
  // than silently returning the wrong thing.
  EmbedResult wait_for_embed(uint64_t ticket);

  // Synchronous show(), deliberately outside the queue: /api/show reads a
  // manifest rather than loading the model, so it costs Ollama no inference
  // capacity and should not wait behind a chat.
  ShowResult show(std::string_view model, bool verbose = false) const;

  ~OllamaClient();

private:
  OllamaClient() = default;  // Use instance() for access.

  // The model and host a job runs against, read once when the job is enqueued.
  // Workers never look at shared configuration, which is what makes a
  // configure() during a run safe.
  struct Target {
    std::string host;
    std::string model;
  };

  Target chat_target() const;
  Target embed_target(std::string_view model) const;
  std::string host() const;

  // Starts the worker pool if it is not running yet. Called from both
  // enqueue_*() paths, never from show() or context_length().
  void ensure_workers();
  void worker_loop();

  struct ChatJob {
    uint64_t ticket = 0;
    Target target;
    std::vector<ChatMessage> messages;
    std::vector<std::string> tools;
    std::promise<ChatResult> promise;
  };

  struct EmbedJob {
    uint64_t ticket = 0;
    Target target;
    std::vector<std::string> input;
    std::promise<EmbedResult> promise;
  };

  mutable std::mutex mConfigMutex;
  std::string mHost = "http://localhost:11434";
  std::string mModel;
  std::string mEmbedModel = kDefaultEmbedModel;

  std::deque<ChatJob> mChatQueue;
  std::deque<EmbedJob> mEmbedQueue;
  std::mutex mQueueMutex;
  std::condition_variable mQueueCv;
  bool mShutdown{false};

  std::atomic<int64_t> mNumCtx{0};
  std::atomic<int> mConcurrency{kDefaultOllamaJobs};
  std::atomic<uint64_t> mNextTicket{0};

  std::unordered_map<uint64_t, std::future<ChatResult>> mChatResults;
  std::unordered_map<uint64_t, std::future<EmbedResult>> mEmbedResults;
  std::mutex mResultsMutex;

  std::once_flag mWorkersOnce;
  std::atomic<bool> mWorkersStarted{false};
  std::vector<std::thread> mWorkers;
};

}  // namespace agent

#endif  // OLLAMA_CLIENT_H
