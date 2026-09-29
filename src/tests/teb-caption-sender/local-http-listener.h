#ifndef LOCAL_HTTP_LISTENER_H
#define LOCAL_HTTP_LISTENER_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One HTTP request the listener received. Header names are lower case.
struct RecordedRequest {
	std::string method;
	std::string path;
	std::map<std::string, std::string> headers;
	std::string body;
};

// An HTTP/1.1 listener on 127.0.0.1 that serves one connection at a time. It
// records each request, then answers with the set status and closes. While it
// holds, it answers nothing until release() or its destructor.
class LocalHttpListener {
public:
	LocalHttpListener();
	~LocalHttpListener();

	LocalHttpListener(const LocalHttpListener &) = delete;
	LocalHttpListener &operator=(const LocalHttpListener &) = delete;

	std::string url() const;
	void set_status(int status);
	void hold();
	void release();
	std::vector<RecordedRequest> requests();
	bool wait_for_requests(size_t count, std::chrono::milliseconds timeout);

private:
	void run();
	void serve(std::uintptr_t client);

	int port_ = 0;
	std::uintptr_t listener_ = 0;
	std::mutex mutex_;
	std::condition_variable changed_;
	std::vector<RecordedRequest> requests_;
	int status_ = 200;
	bool holding_ = false;
	std::atomic<bool> stopping_{false};
	std::thread thread_;
};

#endif
