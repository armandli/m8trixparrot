#include <core/ollama_client.h>

#include <algorithm>
#include <optional>

namespace agent {

namespace {

// Shared by wait_for() and wait_for_embed(): both result types carry an
// `error` string and default to not-ok, so the bookkeeping is identical.
template<typename Result>
Result take_result(std::unordered_map<uint64_t, std::future<Result>>& results,
                   std::mutex& results_mutex, uint64_t ticket) {
  std::future<Result> future;
  {
    std::lock_guard<std::mutex> lk(results_mutex);
    auto it = results.find(ticket);
    if (it == results.end()) {
      Result err;
      err.error = "no pending request for ticket " + std::to_string(ticket);
      return err;
    }
    future = std::move(it->second);
    results.erase(it);
  }

  // A worker that went away without fulfilling its promise breaks the future.
  // That is a bug rather than an expected path, but it surfaces on an agent
  // thread, where an escaping exception would take the whole turn down.
  try {
    return future.get();
  } catch (const std::exception& e) {
    Result err;
    err.error = std::string("the ollama request was abandoned: ") + e.what();
    return err;
  }
}

}  // namespace

OllamaClient::~OllamaClient() {
  {
    std::lock_guard<std::mutex> lk(mQueueMutex);
    mShutdown = true;
  }
  mQueueCv.notify_all();
  for (std::thread& worker : mWorkers) {
    if (worker.joinable()) worker.join();
  }
}

OllamaClient& OllamaClient::instance() {
  static OllamaClient inst;
  return inst;
}

void OllamaClient::configure(const std::string& model, const std::string& host) {
  OllamaClient& inst = instance();
  std::lock_guard<std::mutex> lk(inst.mConfigMutex);
  inst.mModel = model;
  inst.mHost = host;
}

void OllamaClient::configure_embed(const std::string& embed_model,
                                   const std::string& host) {
  OllamaClient& inst = instance();
  std::lock_guard<std::mutex> lk(inst.mConfigMutex);
  if (not embed_model.empty()) inst.mEmbedModel = embed_model;
  if (not host.empty()) inst.mHost = host;
}

void OllamaClient::set_concurrency(int jobs) {
  OllamaClient& inst = instance();
  // Ignored once the pool is up, so that concurrency() never claims a size the
  // running pool does not have.
  if (inst.mWorkersStarted.load()) return;
  inst.mConcurrency.store(std::max(1, jobs));
}

void OllamaClient::set_num_ctx(int64_t num_ctx) {
  instance().mNumCtx.store(num_ctx);
}

std::string OllamaClient::host() const {
  std::lock_guard<std::mutex> lk(mConfigMutex);
  return mHost;
}

OllamaClient::Target OllamaClient::chat_target() const {
  std::lock_guard<std::mutex> lk(mConfigMutex);
  return Target{mHost, mModel};
}

OllamaClient::Target OllamaClient::embed_target(std::string_view model) const {
  std::lock_guard<std::mutex> lk(mConfigMutex);
  return Target{mHost, model.empty() ? mEmbedModel : std::string(model)};
}

int64_t OllamaClient::context_length(std::string_view model) const {
  const BasicOllamaClient client(host());
  const ShowResult shown = client.show(model);
  if (not shown.ok) return 0;
  const int64_t from_info =
      context_length_from_model_info(shown.model_info.text);
  if (from_info > 0) return from_info;
  if (shown.model_params and shown.model_params->num_ctx) {
    return *shown.model_params->num_ctx;
  }
  return 0;
}

ShowResult OllamaClient::show(std::string_view model, bool verbose) const {
  const BasicOllamaClient client(host());
  return client.show(model, verbose);
}

uint64_t OllamaClient::enqueue_chat(const std::vector<ChatMessage>& messages,
                                    const std::vector<std::string>& tools) {
  ensure_workers();

  std::promise<ChatResult> promise;
  const uint64_t ticket = mNextTicket.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(mResultsMutex);
    mChatResults[ticket] = promise.get_future();
  }
  {
    std::lock_guard<std::mutex> lk(mQueueMutex);
    mChatQueue.push_back(
        ChatJob{ticket, chat_target(), messages, tools, std::move(promise)});
  }
  mQueueCv.notify_one();
  return ticket;
}

ChatResult OllamaClient::wait_for(uint64_t ticket) {
  return take_result(mChatResults, mResultsMutex, ticket);
}

uint64_t OllamaClient::enqueue_embed(const std::vector<std::string>& input,
                                     std::string_view model) {
  ensure_workers();

  std::promise<EmbedResult> promise;
  const uint64_t ticket = mNextTicket.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lk(mResultsMutex);
    mEmbedResults[ticket] = promise.get_future();
  }
  {
    std::lock_guard<std::mutex> lk(mQueueMutex);
    mEmbedQueue.push_back(
        EmbedJob{ticket, embed_target(model), input, std::move(promise)});
  }
  mQueueCv.notify_one();
  return ticket;
}

EmbedResult OllamaClient::wait_for_embed(uint64_t ticket) {
  return take_result(mEmbedResults, mResultsMutex, ticket);
}

void OllamaClient::ensure_workers() {
  std::call_once(mWorkersOnce, [this] {
    // Flagged before the value is read, so a set_concurrency() that returns
    // after this point is ignored rather than silently lost.
    mWorkersStarted.store(true);
    const int jobs = std::max(1, mConcurrency.load());
    mWorkers.reserve(static_cast<size_t>(jobs));
    for (int i = 0; i < jobs; ++i) {
      mWorkers.emplace_back([this] { worker_loop(); });
    }
  });
}

void OllamaClient::worker_loop() {
  // One client per worker: BasicOllamaClient documents itself as unsafe for
  // concurrent calls on a single instance, and a worker that owns its own can
  // later hold a keep-alive curl handle without sharing it.
  BasicOllamaClient client;

  while (true) {
    std::optional<ChatJob> chat;
    std::optional<EmbedJob> embed;
    {
      std::unique_lock<std::mutex> lk(mQueueMutex);
      mQueueCv.wait(lk, [this] {
        return not mEmbedQueue.empty() or not mChatQueue.empty() or mShutdown;
      });

      // Embeddings jump the queue — see the class comment for why that cannot
      // starve chat.
      if (not mEmbedQueue.empty()) {
        embed = std::move(mEmbedQueue.front());
        mEmbedQueue.pop_front();
      } else if (not mChatQueue.empty()) {
        chat = std::move(mChatQueue.front());
        mChatQueue.pop_front();
      } else {
        // Shutting down, and both queues are drained. Returning any earlier
        // would leave a queued job's promise broken under a blocked waiter.
        return;
      }
    }

    if (embed) {
      client.mHost = embed->target.host;
      embed->promise.set_value(client.embed(embed->target.model, embed->input));
    } else {
      client.mHost = chat->target.host;
      chat->promise.set_value(client.chat(chat->target.model, chat->messages,
                                          chat->tools, mNumCtx.load()));
    }
  }
}

}  // namespace agent
