// The OllamaClient singleton: chat responses carry Ollama's token counts, the
// model's context length is read from /api/show, embeddings share the work pool
// with chat, and the pool never has more requests in flight than it was capped
// at. A LoopbackServer stands in for Ollama — the path is ignored, so one canned
// body answers whatever the client asks.

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/oc/basic_ollama_client.h>
#include <core/oc/ollama_client.h>
#include <loopback_server.h>

namespace agent {
namespace {

// One body that satisfies both parsers, because a concurrent LoopbackServer
// hands out canned bodies in arrival order and a chat and an embed racing each
// other have no fixed arrival order. The keys are ordered so that every reader
// — chat's message pass, chat's token-count pass, and embed — walks strictly
// forwards through the object.
const char* kChatAndEmbedBody =
    R"({"model":"m","message":{"role":"assistant","content":"hi"},)"
    R"("embeddings":[[3.0,4.0]],"total_duration":0,"load_duration":0,)"
    R"("prompt_eval_count":1,"eval_count":1,"done":true})";

TEST(OllamaClientTest, ChatResultCarriesTokenCounts) {
  test::LoopbackServer server(
      200, "application/json",
      R"({"model":"m","message":{"role":"assistant","content":"hi"},)"
      R"("done":true,"prompt_eval_count":1234,"eval_count":56})");
  oc::OllamaClient::configure("m", server.url(""));
  oc::OllamaClient::set_num_ctx(0);

  const uint64_t ticket = oc::OllamaClient::instance().enqueue_chat(
      {oc::ChatMessage{"user", "hello", {}, ""}}, {});
  const oc::ChatResult reply = oc::OllamaClient::instance().wait_for(ticket);

  ASSERT_TRUE(reply.ok) << reply.error;
  EXPECT_EQ("hi", reply.content);
  EXPECT_EQ(1234, reply.prompt_eval_count);
  EXPECT_EQ(56, reply.eval_count);
}

TEST(OllamaClientTest, DetectsContextLengthFromModelInfo) {
  test::LoopbackServer server(
      200, "application/json",
      R"({"model_info":{"general.architecture":"gemma3",)"
      R"("gemma3.context_length":262144,"gemma3.embedding_length":5376}})");
  oc::OllamaClient::configure("m", server.url(""));

  EXPECT_EQ(262144, oc::OllamaClient::instance().context_length("m"));
}

TEST(OllamaClientTest, ContextLengthIsZeroWhenAbsent) {
  test::LoopbackServer server(
      200, "application/json",
      R"({"model_info":{"general.architecture":"x"}})");
  oc::OllamaClient::configure("m", server.url(""));

  EXPECT_EQ(0, oc::OllamaClient::instance().context_length("m"));
}

TEST(OllamaClientTest, EmbedGoesThroughTheQueue) {
  test::LoopbackServer server(
      200, "application/json",
      R"({"model":"e","embeddings":[[0.25,0.5,0.75]]})");
  oc::OllamaClient::configure("m", server.url(""));
  oc::OllamaClient::configure_embed("e");

  oc::OllamaClient& client = oc::OllamaClient::instance();
  const oc::EmbedResult embedded = client.wait_for_embed(client.enqueue_embed({"hi"}));

  ASSERT_TRUE(embedded.ok) << embedded.error;
  ASSERT_EQ(1u, embedded.embeddings.size());
  EXPECT_EQ(3u, embedded.embeddings.front().size());
  EXPECT_DOUBLE_EQ(0.25, embedded.embeddings.front()[0]);
}

TEST(OllamaClientTest, WaitForEmbedRejectsATicketItDoesNotOwn) {
  test::LoopbackServer server(200, "application/json", kChatAndEmbedBody);
  oc::OllamaClient::configure("m", server.url(""));
  oc::OllamaClient& client = oc::OllamaClient::instance();

  EXPECT_FALSE(client.wait_for_embed(999999).ok);

  // A chat ticket is a real ticket, but not this queue's: tickets come from one
  // counter so the mix-up is reported rather than answered with the wrong type.
  const uint64_t chat_ticket = client.enqueue_chat({oc::ChatMessage{"user", "x", {}, ""}});
  EXPECT_FALSE(client.wait_for_embed(chat_ticket).ok);
  EXPECT_TRUE(client.wait_for(chat_ticket).ok);
}

// Both scheduling cases share one server and one enqueue burst: the pool starts
// once per process and its size is fixed from then on, so a second case setting
// a different cap would be quietly ignored.
TEST(OllamaClientTest, TheWorkPoolCapsRequestsInFlightAndRunsEmbedsFirst) {
  constexpr auto kHold = std::chrono::milliseconds(150);
  test::LoopbackServer server(test::LoopbackOptions{
      {kChatAndEmbedBody}, kHold, /*concurrent=*/true});
  oc::OllamaClient::configure("m", server.url(""));
  oc::OllamaClient::configure_embed("e");

  oc::OllamaClient& client = oc::OllamaClient::instance();
  const int jobs = client.concurrency();
  ASSERT_GE(jobs, 1);

  // Enough chats to fill every worker twice over, so the last one can only be
  // picked up after a full round has finished.
  std::vector<uint64_t> chats;
  for (int i = 0; i < jobs * 2 + 1; ++i) {
    chats.push_back(client.enqueue_chat({oc::ChatMessage{"user", "x", {}, ""}}));
  }
  // Enqueued last, so being served early can only be the queue's doing.
  const uint64_t embed_ticket = client.enqueue_embed({"hi"});

  const auto start = std::chrono::steady_clock::now();
  const oc::EmbedResult embedded = client.wait_for_embed(embed_ticket);
  const auto embed_done = std::chrono::steady_clock::now();
  ASSERT_TRUE(embedded.ok) << embedded.error;

  for (const uint64_t ticket : chats) {
    EXPECT_TRUE(client.wait_for(ticket).ok);
  }
  const auto all_done = std::chrono::steady_clock::now();

  EXPECT_EQ(static_cast<std::size_t>(jobs), server.max_concurrent());

  // The embed waits out at most the round already running, never the chats
  // queued ahead of it, which need at least one round more.
  EXPECT_LT(embed_done - start, 2 * kHold);
  EXPECT_GT(all_done - start, 2 * kHold);
}

TEST(ContextLengthFromModelInfoTest, MatchesArchPrefixedKey) {
  EXPECT_EQ(40960, oc::context_length_from_model_info(
                       R"({"qwen3.context_length":40960})"));
  EXPECT_EQ(8192, oc::context_length_from_model_info(
                      R"({"context_length":8192})"));
  EXPECT_EQ(0, oc::context_length_from_model_info(R"({"foo":1})"));
  EXPECT_EQ(0, oc::context_length_from_model_info(""));
}

}  // namespace
}  // namespace agent
