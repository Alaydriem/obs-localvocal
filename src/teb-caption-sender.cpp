#include "teb-caption-sender.h"

#include <curl/curl.h>

#include <cstdio>
#include <utility>

namespace {

const char *const CAPTION_PATH = "/caption";
const char *const REPLACEMENT_CHARACTER = "\xEF\xBF\xBD";

std::once_flag curl_init_once;

size_t discard_response(char *, size_t size, size_t count, void *)
{
	return size * count;
}

// libcurl calls this during a transfer; a non-zero answer aborts it.
int abort_when_stopping(void *abort_flag, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
	return static_cast<std::atomic<bool> *>(abort_flag)->load() ? 1 : 0;
}

void append_json_string(std::string &out, const std::string &value)
{
	out += '"';
	for (unsigned char c : value) {
		switch (c) {
		case '"':
			out += "\\\"";
			break;
		case '\\':
			out += "\\\\";
			break;
		case '\b':
			out += "\\b";
			break;
		case '\f':
			out += "\\f";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\t':
			out += "\\t";
			break;
		default:
			if (c < 0x20) {
				char escaped[7];
				std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
				out += escaped;
			} else {
				out += static_cast<char>(c);
			}
		}
	}
	out += '"';
}

size_t utf8_length(unsigned char lead)
{
	if (lead < 0x80)
		return 1;
	if ((lead >> 5) == 0x6)
		return 2;
	if ((lead >> 4) == 0xE)
		return 3;
	if ((lead >> 3) == 0x1E)
		return 4;
	return 0;
}

// Refuses a cut sequence, a stray continuation byte, an overlong form, a UTF-16
// surrogate and a code point above U+10FFFF.
bool utf8_sequence_is_valid(const std::string &text, size_t at, size_t length)
{
	if (length == 0 || at + length > text.size())
		return false;
	for (size_t k = 1; k < length; k++) {
		if ((static_cast<unsigned char>(text[at + k]) >> 6) != 0x2)
			return false;
	}
	const unsigned char lead = static_cast<unsigned char>(text[at]);
	const unsigned char next = length > 1 ? static_cast<unsigned char>(text[at + 1]) : 0;
	switch (length) {
	case 2:
		return lead >= 0xC2;
	case 3:
		return !(lead == 0xE0 && next < 0xA0) && !(lead == 0xED && next >= 0xA0);
	case 4:
		return lead <= 0xF4 && !(lead == 0xF0 && next < 0x90) &&
		       !(lead == 0xF4 && next >= 0x90);
	default:
		return true;
	}
}

bool post_line(CURL *curl, const std::string &url, const std::string &token,
	       const std::string &body, std::atomic<bool> &abort_flag, std::string &error)
{
	curl_easy_reset(curl);
	struct curl_slist *headers = curl_slist_append(nullptr, "Content-Type: application/json");
	if (!token.empty()) {
		const std::string authorization = "Authorization: Bearer " + token;
		headers = curl_slist_append(headers, authorization.c_str());
	}
	curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
	curl_easy_setopt(curl, CURLOPT_PROXY, "");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, TebCaptionSender::REQUEST_TIMEOUT_MS);
	curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_response);
	curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, abort_when_stopping);
	curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &abort_flag);
	const CURLcode code = curl_easy_perform(curl);
	long status = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_slist_free_all(headers);
	if (code != CURLE_OK) {
		error = curl_easy_strerror(code);
		return false;
	}
	if (status < 200 || status > 299) {
		error = "HTTP " + std::to_string(status);
		return false;
	}
	return true;
}

} // namespace

TebCaptionSender::TebCaptionSender(WarningCallback warn) : warn_(std::move(warn))
{
	std::call_once(curl_init_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
	worker_ = std::thread([this] { run(); });
}

TebCaptionSender::~TebCaptionSender()
{
	stop();
}

void TebCaptionSender::configure(bool enabled, const std::string &url, const std::string &token)
{
	std::lock_guard<std::mutex> lock(mutex_);
	enabled_ = enabled;
	url_ = url;
	token_ = token;
	if (!enabled_ || url_.empty())
		queue_.clear();
}

void TebCaptionSender::set_source(const std::string &source)
{
	std::lock_guard<std::mutex> lock(mutex_);
	source_ = label(source);
}

bool TebCaptionSender::enqueue(const std::string &text, const std::string &language)
{
	bool dropped_oldest = false;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (stopping_ || !enabled_ || url_.empty() || text.empty())
			return false;
		if (queue_.size() >= QUEUE_LIMIT) {
			queue_.pop_front();
			dropped_oldest = true;
		}
		queue_.push_back({text, label(language), source_});
	}
	wake_.notify_one();
	if (dropped_oldest) {
		dropped_++;
		warn_("teb caption queue is full; the oldest line is dropped");
	}
	return true;
}

void TebCaptionSender::stop()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		stopping_ = true;
		queue_.clear();
	}
	abort_.store(true);
	wake_.notify_all();
	if (worker_.joinable())
		worker_.join();
}

uint64_t TebCaptionSender::sent() const
{
	return sent_.load();
}

uint64_t TebCaptionSender::failed() const
{
	return failed_.load();
}

uint64_t TebCaptionSender::dropped() const
{
	return dropped_.load();
}

std::string TebCaptionSender::json_body(const TebCaptionLine &line)
{
	std::string body = "{\"text\":";
	append_json_string(body, valid_utf8(line.text));
	body += ",\"final\":true";
	if (!line.language.empty()) {
		body += ",\"language\":";
		append_json_string(body, valid_utf8(line.language));
	}
	if (!line.source.empty()) {
		body += ",\"source\":";
		append_json_string(body, valid_utf8(line.source));
	}
	body += '}';
	return body;
}

std::string TebCaptionSender::endpoint(const std::string &base_url)
{
	const std::string path = CAPTION_PATH;
	const size_t first = base_url.find_first_not_of(" \t");
	if (first == std::string::npos)
		return path;
	std::string url = base_url.substr(first);
	while (!url.empty() && (url.back() == '/' || url.back() == ' ' || url.back() == '\t'))
		url.pop_back();
	if (url.size() >= path.size() &&
	    url.compare(url.size() - path.size(), path.size(), path) == 0)
		return url;
	return url + path;
}

std::string TebCaptionSender::valid_utf8(const std::string &text)
{
	std::string out;
	out.reserve(text.size());
	size_t at = 0;
	while (at < text.size()) {
		const size_t length = utf8_length(static_cast<unsigned char>(text[at]));
		if (utf8_sequence_is_valid(text, at, length)) {
			out.append(text, at, length);
			at += length;
		} else {
			out += REPLACEMENT_CHARACTER;
			at += 1;
		}
	}
	return out;
}

std::string TebCaptionSender::label(const std::string &value)
{
	const std::string text = valid_utf8(value);
	std::string out;
	size_t kept = 0;
	size_t at = 0;
	while (at < text.size() && kept < MAX_LABEL_CHARS) {
		const unsigned char lead = static_cast<unsigned char>(text[at]);
		const size_t length = utf8_length(lead);
		const unsigned char next = length > 1 ? static_cast<unsigned char>(text[at + 1])
						      : 0;
		// C0 controls, DEL, and the C1 controls U+0080 to U+009F.
		const bool control = lead < 0x20 || lead == 0x7F || (lead == 0xC2 && next < 0xA0);
		if (!control) {
			out.append(text, at, length);
			kept++;
		}
		at += length;
	}
	return out;
}

void TebCaptionSender::run()
{
	CURL *curl = curl_easy_init();
	for (;;) {
		TebCaptionLine line;
		std::string url;
		std::string token;
		{
			std::unique_lock<std::mutex> lock(mutex_);
			wake_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
			if (stopping_)
				break;
			line = std::move(queue_.front());
			queue_.pop_front();
			url = endpoint(url_);
			token = token_;
		}
		std::string error = "libcurl did not start";
		if (curl != nullptr &&
		    post_line(curl, url, token, json_body(line), abort_, error)) {
			sent_++;
			continue;
		}
		if (abort_.load())
			break;
		failed_++;
		warn_("teb caption not sent to " + url + ": " + error + "; the line is dropped");
	}
	if (curl != nullptr)
		curl_easy_cleanup(curl);
}
