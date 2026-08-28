// The OllamaClient singleton: chat responses carry Ollama's token counts, and
// the model's context length is read from /api/show. A LoopbackServer stands in
// for Ollama — the path is ignored, so one canned body answers whatever the
// client asks.

#include <cstdint>
#include <string>

#include <gtest/gtest.h>

#include <core/basic_ollama_client.h>
#include <core/ollama_client.h>
#include <loopback_server.h>

namespace agent {
namespace {

TEST(OllamaClientTest, ChatResultCarriesTokenCounts) {
  test::LoopbackServer server(
      200, "application/json",
      R"({"model":"m","message":{"role":"assistant","content":"hi"},)"
      R"("done":true,"prompt_eval_count":1234,"eval_count":56})");
  OllamaClient::configure("m", server.url(""));
  OllamaClient::set_num_ctx(0);

  const uint64_t ticket = OllamaClient::instance().enqueue_chat(
      {ChatMessage{"user", "hello", {}, ""}}, {});
  const ChatResult reply = OllamaClient::instance().wait_for(ticket);

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
  OllamaClient::configure("m", server.url(""));

  EXPECT_EQ(262144, OllamaClient::instance().context_length("m"));
}

TEST(OllamaClientTest, ContextLengthIsZeroWhenAbsent) {
  test::LoopbackServer server(
      200, "application/json",
      R"({"model_info":{"general.architecture":"x"}})");
  OllamaClient::configure("m", server.url(""));

  EXPECT_EQ(0, OllamaClient::instance().context_length("m"));
}

TEST(ContextLengthFromModelInfoTest, MatchesArchPrefixedKey) {
  EXPECT_EQ(40960, context_length_from_model_info(
                       R"({"qwen3.context_length":40960})"));
  EXPECT_EQ(8192, context_length_from_model_info(
                      R"({"context_length":8192})"));
  EXPECT_EQ(0, context_length_from_model_info(R"({"foo":1})"));
  EXPECT_EQ(0, context_length_from_model_info(""));
}

}  // namespace
}  // namespace agent
