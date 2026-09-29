#ifndef TEB_CAPTION_SENDER_H
#define TEB_CAPTION_SENDER_H

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

// One final caption line for teb's POST /caption.
struct TebCaptionLine {
	std::string text;
	std::string language;
	std::string source;
};

// Sends final caption lines to teb's POST /caption from one worker thread.
// enqueue() never waits on the network. The queue holds at most QUEUE_LIMIT lines
// and drops the oldest line when it is full. Each line is sent once, with a total
// timeout of REQUEST_TIMEOUT_MS. A line that fails is dropped and reported once
// through the warning callback. No warning holds the token.
class TebCaptionSender {
public:
	using WarningCallback = std::function<void(const std::string &)>;

	static constexpr size_t QUEUE_LIMIT = 8;
	static constexpr long REQUEST_TIMEOUT_MS = 1000;

	explicit TebCaptionSender(WarningCallback warn);
	~TebCaptionSender();

	TebCaptionSender(const TebCaptionSender &) = delete;
	TebCaptionSender &operator=(const TebCaptionSender &) = delete;

	// Thread-safe. Sending is off while enabled is false or url is empty.
	void configure(bool enabled, const std::string &url, const std::string &token);
	// Thread-safe. The name of the source the filter is on.
	void set_source(const std::string &source);
	// Thread-safe, and never waits on the network. False when sending is off, the
	// sender has stopped, or the text is empty.
	bool enqueue(const std::string &text, const std::string &language);
	// Stops the worker and joins it. A send in flight is aborted and not reported.
	void stop();

	uint64_t sent() const;
	uint64_t failed() const;
	uint64_t dropped() const;

	static std::string json_body(const TebCaptionLine &line);
	static std::string endpoint(const std::string &base_url);
	static std::string valid_utf8(const std::string &text);

private:
	void run();

	WarningCallback warn_;
	std::mutex mutex_;
	std::condition_variable wake_;
	std::deque<TebCaptionLine> queue_;
	bool enabled_ = false;
	std::string url_;
	std::string token_;
	std::string source_;
	bool stopping_ = false;
	std::atomic<bool> abort_{false};
	std::atomic<uint64_t> sent_{0};
	std::atomic<uint64_t> failed_{0};
	std::atomic<uint64_t> dropped_{0};
	std::thread worker_;
};

#endif
