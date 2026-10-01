#include "local-http-listener.h"
#include "teb-caption-sender.h"

#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static int failures = 0;

#define CHECK(condition)                                                                    \
	do {                                                                                \
		if (!(condition)) {                                                         \
			std::fprintf(stderr, "FAILED line %d: %s\n", __LINE__, #condition); \
			failures++;                                                         \
		}                                                                           \
	} while (0)

namespace {

const std::string TOKEN = "teb-token-0123456789";

struct Warnings {
	std::mutex mutex;
	std::vector<std::string> lines;

	TebCaptionSender::WarningCallback callback()
	{
		return [this](const std::string &line) {
			std::lock_guard<std::mutex> lock(mutex);
			lines.push_back(line);
		};
	}

	std::vector<std::string> copy()
	{
		std::lock_guard<std::mutex> lock(mutex);
		return lines;
	}

	bool hold_the_token()
	{
		for (const auto &line : copy()) {
			if (line.find(TOKEN) != std::string::npos)
				return true;
		}
		return false;
	}
};

template<typename Predicate> bool wait_until(Predicate predicate, std::chrono::milliseconds timeout)
{
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	while (std::chrono::steady_clock::now() < deadline) {
		if (predicate())
			return true;
		std::this_thread::sleep_for(10ms);
	}
	return predicate();
}

} // namespace

static void a_line_is_posted_as_json_with_the_bearer_token()
{
	LocalHttpListener listener;
	Warnings warnings;
	TebCaptionSender sender(warnings.callback());
	sender.configure(true, listener.url(), TOKEN);
	sender.set_source("Mic/Aux");
	CHECK(sender.enqueue("and that is how you decode a stream key", "en"));
	CHECK(wait_until([&] { return sender.sent() == 1; }, 3000ms));
	auto requests = listener.requests();
	CHECK(requests.size() == 1);
	if (requests.size() == 1) {
		CHECK(requests[0].method == "POST");
		CHECK(requests[0].path == "/caption");
		CHECK(requests[0].headers["authorization"] == "Bearer " + TOKEN);
		CHECK(requests[0].headers["content-type"] == "application/json");
		CHECK(requests[0].body ==
		      "{\"text\":\"and that is how you decode a stream key\",\"final\":true,"
		      "\"language\":\"en\",\"source\":\"Mic/Aux\"}");
	}
	CHECK(warnings.copy().empty());
}

static void quotes_backslashes_and_control_characters_are_escaped()
{
	TebCaptionLine line{"he said \"hi\" \\ \n\t\x01 \xC3\xA9", "", ""};
	CHECK(TebCaptionSender::json_body(line) ==
	      "{\"text\":\"he said \\\"hi\\\" \\\\ \\n\\t\\u0001 \xC3\xA9\",\"final\":true}");
}

static void invalid_utf8_is_replaced_before_the_json_is_built()
{
	CHECK(TebCaptionSender::valid_utf8("caf\xC3 ok") == "caf\xEF\xBF\xBD ok");
	CHECK(TebCaptionSender::valid_utf8("\xC3\xA9t\xC3\xA9") == "\xC3\xA9t\xC3\xA9");
	CHECK(TebCaptionSender::valid_utf8("\xC0\x80") == "\xEF\xBF\xBD\xEF\xBF\xBD");
	TebCaptionLine line{"cut \xE2\x82", "en", ""};
	CHECK(TebCaptionSender::json_body(line) ==
	      "{\"text\":\"cut \xEF\xBF\xBD\xEF\xBF\xBD\",\"final\":true,\"language\":\"en\"}");
}

static void the_endpoint_ends_in_caption_once()
{
	CHECK(TebCaptionSender::endpoint("http://10.57.2.6:8088") ==
	      "http://10.57.2.6:8088/caption");
	CHECK(TebCaptionSender::endpoint(" http://10.57.2.6:8088/ ") ==
	      "http://10.57.2.6:8088/caption");
	CHECK(TebCaptionSender::endpoint("http://10.57.2.6:8088/caption") ==
	      "http://10.57.2.6:8088/caption");
}

static void nothing_is_queued_while_sending_is_off()
{
	LocalHttpListener listener;
	Warnings warnings;
	TebCaptionSender sender(warnings.callback());
	CHECK(!sender.enqueue("before configure", "en"));
	sender.configure(false, listener.url(), TOKEN);
	CHECK(!sender.enqueue("while off", "en"));
	sender.configure(true, "", TOKEN);
	CHECK(!sender.enqueue("with no url", "en"));
	sender.configure(true, listener.url(), TOKEN);
	CHECK(!sender.enqueue("", "en"));
	std::this_thread::sleep_for(200ms);
	CHECK(listener.requests().empty());
}

static void a_server_that_does_not_answer_fails_the_line_within_the_timeout()
{
	LocalHttpListener listener;
	listener.hold();
	Warnings warnings;
	TebCaptionSender sender(warnings.callback());
	sender.configure(true, listener.url(), TOKEN);
	const auto start = std::chrono::steady_clock::now();
	CHECK(sender.enqueue("nobody answers", "en"));
	CHECK(wait_until([&] { return sender.failed() == 1; }, 3000ms));
	const auto elapsed = std::chrono::steady_clock::now() - start;
	CHECK(elapsed >= 900ms);
	CHECK(elapsed < 2000ms);
	CHECK(warnings.copy().size() == 1);
	CHECK(!warnings.hold_the_token());
	listener.release();
	CHECK(sender.enqueue("the next line", "en"));
	CHECK(wait_until([&] { return sender.sent() == 1; }, 3000ms));
}

static void a_non_2xx_answer_is_one_failure_and_no_retry()
{
	LocalHttpListener listener;
	listener.set_status(500);
	Warnings warnings;
	TebCaptionSender sender(warnings.callback());
	sender.configure(true, listener.url(), TOKEN);
	CHECK(sender.enqueue("refused", "en"));
	CHECK(wait_until([&] { return sender.failed() == 1; }, 3000ms));
	std::this_thread::sleep_for(300ms);
	CHECK(listener.requests().size() == 1);
	const auto lines = warnings.copy();
	CHECK(lines.size() == 1);
	CHECK(lines.size() == 1 && lines[0].find("HTTP 500") != std::string::npos);
	CHECK(!warnings.hold_the_token());
}

static void a_full_queue_drops_the_oldest_line()
{
	LocalHttpListener listener;
	listener.hold();
	Warnings warnings;
	TebCaptionSender sender(warnings.callback());
	sender.configure(true, listener.url(), TOKEN);
	CHECK(sender.enqueue("line 1", "en"));
	CHECK(listener.wait_for_requests(1, 2000ms));
	for (int n = 2; n <= 10; n++)
		CHECK(sender.enqueue("line " + std::to_string(n), "en"));
	listener.release();
	CHECK(listener.wait_for_requests(9, 5000ms));
	CHECK(sender.dropped() == 1);
	std::vector<std::string> expected;
	for (int n : {1, 3, 4, 5, 6, 7, 8, 9, 10})
		expected.push_back(
			TebCaptionSender::json_body({"line " + std::to_string(n), "en", ""}));
	std::vector<std::string> bodies;
	for (const auto &request : listener.requests())
		bodies.push_back(request.body);
	CHECK(bodies == expected);
}

static void stop_during_a_send_returns_within_the_timeout_and_warns_nothing()
{
	LocalHttpListener listener;
	listener.hold();
	Warnings warnings;
	auto sender = std::make_unique<TebCaptionSender>(warnings.callback());
	sender->configure(true, listener.url(), TOKEN);
	CHECK(sender->enqueue("a line in flight", "en"));
	CHECK(listener.wait_for_requests(1, 2000ms));
	const auto start = std::chrono::steady_clock::now();
	sender.reset();
	const auto elapsed = std::chrono::steady_clock::now() - start;
	CHECK(elapsed < 1500ms);
	CHECK(warnings.copy().empty());
}

static void a_long_or_odd_label_is_cut_to_what_teb_accepts()
{
	// teb refuses the whole line when language or source holds more than 64
	// characters or a control character.
	CHECK(TebCaptionSender::label(std::string(70, 'x')) == std::string(64, 'x'));
	std::string wide;
	for (int n = 0; n < 70; n++)
		wide += "\xC3\xA9";
	std::string wide_64;
	for (int n = 0; n < 64; n++)
		wide_64 += "\xC3\xA9";
	CHECK(TebCaptionSender::label(wide) == wide_64);
	CHECK(TebCaptionSender::label("Mic\nAux\t1\x7F\xC2\x85") == "MicAux1");
	LocalHttpListener listener;
	Warnings warnings;
	TebCaptionSender sender(warnings.callback());
	sender.configure(true, listener.url(), TOKEN);
	sender.set_source(std::string(80, 's'));
	CHECK(sender.enqueue("hello", "en\n"));
	CHECK(wait_until([&] { return sender.sent() == 1; }, 3000ms));
	auto requests = listener.requests();
	CHECK(requests.size() == 1 &&
	      requests[0].body ==
		      TebCaptionSender::json_body({"hello", "en", std::string(64, 's')}));
}

int main()
{
	a_line_is_posted_as_json_with_the_bearer_token();
	quotes_backslashes_and_control_characters_are_escaped();
	invalid_utf8_is_replaced_before_the_json_is_built();
	the_endpoint_ends_in_caption_once();
	nothing_is_queued_while_sending_is_off();
	a_server_that_does_not_answer_fails_the_line_within_the_timeout();
	a_non_2xx_answer_is_one_failure_and_no_retry();
	a_full_queue_drops_the_oldest_line();
	stop_during_a_send_returns_within_the_timeout_and_warns_nothing();
	a_long_or_odd_label_is_cut_to_what_teb_accepts();
	if (failures == 0)
		std::printf("teb-caption-sender: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
