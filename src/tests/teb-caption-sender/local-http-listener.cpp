#include "local-http-listener.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <cctype>
#include <cstdlib>

namespace {

#ifdef _WIN32
using socket_handle = SOCKET;
const socket_handle NO_SOCKET = INVALID_SOCKET;
const int SEND_FLAGS = 0;
void close_socket(socket_handle handle)
{
	closesocket(handle);
}
#else
using socket_handle = int;
const socket_handle NO_SOCKET = -1;
const int SEND_FLAGS = MSG_NOSIGNAL;
void close_socket(socket_handle handle)
{
	close(handle);
}
#endif

std::string lower(std::string text)
{
	for (char &c : text)
		c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	return text;
}

std::string trim(const std::string &text)
{
	const size_t first = text.find_first_not_of(" \t");
	if (first == std::string::npos)
		return "";
	const size_t last = text.find_last_not_of(" \t\r");
	return text.substr(first, last - first + 1);
}

RecordedRequest parse_head(const std::string &head)
{
	RecordedRequest request;
	size_t line_start = 0;
	bool first_line = true;
	while (line_start < head.size()) {
		size_t line_end = head.find("\r\n", line_start);
		if (line_end == std::string::npos)
			line_end = head.size();
		const std::string line = head.substr(line_start, line_end - line_start);
		if (first_line) {
			const size_t space = line.find(' ');
			const size_t second = line.find(' ', space + 1);
			request.method = line.substr(0, space);
			request.path = line.substr(space + 1, second - space - 1);
			first_line = false;
		} else {
			const size_t colon = line.find(':');
			if (colon != std::string::npos)
				request.headers[lower(trim(line.substr(0, colon)))] =
					trim(line.substr(colon + 1));
		}
		line_start = line_end + 2;
	}
	return request;
}

} // namespace

LocalHttpListener::LocalHttpListener()
{
#ifdef _WIN32
	WSADATA data;
	WSAStartup(MAKEWORD(2, 2), &data);
#endif
	const socket_handle listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address));
	listen(listener, 16);
	socklen_t length = sizeof(address);
	getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length);
	port_ = ntohs(address.sin_port);
	listener_ = static_cast<std::uintptr_t>(listener);
	thread_ = std::thread([this] { run(); });
}

LocalHttpListener::~LocalHttpListener()
{
	stopping_.store(true);
	{
		std::lock_guard<std::mutex> lock(mutex_);
		holding_ = false;
	}
	changed_.notify_all();
	thread_.join();
	close_socket(static_cast<socket_handle>(listener_));
#ifdef _WIN32
	WSACleanup();
#endif
}

std::string LocalHttpListener::url() const
{
	return "http://127.0.0.1:" + std::to_string(port_);
}

void LocalHttpListener::set_status(int status)
{
	std::lock_guard<std::mutex> lock(mutex_);
	status_ = status;
}

void LocalHttpListener::hold()
{
	std::lock_guard<std::mutex> lock(mutex_);
	holding_ = true;
}

void LocalHttpListener::release()
{
	{
		std::lock_guard<std::mutex> lock(mutex_);
		holding_ = false;
	}
	changed_.notify_all();
}

std::vector<RecordedRequest> LocalHttpListener::requests()
{
	std::lock_guard<std::mutex> lock(mutex_);
	return requests_;
}

bool LocalHttpListener::wait_for_requests(size_t count, std::chrono::milliseconds timeout)
{
	std::unique_lock<std::mutex> lock(mutex_);
	return changed_.wait_for(lock, timeout, [&] { return requests_.size() >= count; });
}

void LocalHttpListener::run()
{
	const socket_handle listener = static_cast<socket_handle>(listener_);
	while (!stopping_.load()) {
		fd_set ready;
		FD_ZERO(&ready);
		FD_SET(listener, &ready);
		timeval wait{0, 50000};
		if (select(static_cast<int>(listener) + 1, &ready, nullptr, nullptr, &wait) <= 0)
			continue;
		const socket_handle client = accept(listener, nullptr, nullptr);
		if (client == NO_SOCKET)
			continue;
		serve(static_cast<std::uintptr_t>(client));
	}
}

void LocalHttpListener::serve(std::uintptr_t handle)
{
	const socket_handle client = static_cast<socket_handle>(handle);
	std::string data;
	char buffer[4096];
	size_t head_end = std::string::npos;
	while (head_end == std::string::npos) {
		const int got =
			static_cast<int>(recv(client, buffer, static_cast<int>(sizeof(buffer)), 0));
		if (got <= 0) {
			close_socket(client);
			return;
		}
		data.append(buffer, static_cast<size_t>(got));
		head_end = data.find("\r\n\r\n");
	}
	RecordedRequest request = parse_head(data.substr(0, head_end));
	const size_t length = std::strtoul(request.headers["content-length"].c_str(), nullptr, 10);
	std::string body = data.substr(head_end + 4);
	while (body.size() < length) {
		const int got =
			static_cast<int>(recv(client, buffer, static_cast<int>(sizeof(buffer)), 0));
		if (got <= 0)
			break;
		body.append(buffer, static_cast<size_t>(got));
	}
	request.body = body;
	int status = 200;
	{
		std::unique_lock<std::mutex> lock(mutex_);
		requests_.push_back(request);
		changed_.notify_all();
		changed_.wait(lock, [this] { return !holding_ || stopping_.load(); });
		status = status_;
	}
	const std::string answer = "HTTP/1.1 " + std::to_string(status) +
				   " Test\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
	send(client, answer.c_str(), static_cast<int>(answer.size()), SEND_FLAGS);
	close_socket(client);
}
