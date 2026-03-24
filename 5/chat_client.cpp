#include "chat.h"
#include "chat_client.h"

#include <cstring>
#include <stdlib.h>
#include <unistd.h>

#include <poll.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>

#include <deque>
#include <string>
#include <cctype>

struct chat_client {
	int socket = -1;
	std::string input_buffer;
	std::deque<chat_message*> messages;

	std::string output_buffer;
	size_t output_sent = 0;

	#if NEED_AUTHOR
    std::string name;
    bool name_sent = false;
	#endif	
};

static std::string trim(const std::string& s) {
	size_t l = 0;
	while (l < s.size() && isspace((unsigned char)s[l])) l++;

	size_t r = s.size();
	while (r > l && isspace((unsigned char)s[r - 1])) r--;

	return s.substr(l, r - l);
}

static void flush_output(chat_client *client)
{
    if (!client || client->socket < 0)
        return;

    while (client->output_sent < client->output_buffer.size()) {
        ssize_t n = send(
            client->socket,
            client->output_buffer.data() + client->output_sent,
            client->output_buffer.size() - client->output_sent,
            0
        );

        if (n > 0) {
            client->output_sent += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            break;
        } else {
            close(client->socket);
            client->socket = -1;
            return;
        }
    }

    if (client->output_sent == client->output_buffer.size()) {
        client->output_buffer.clear();
        client->output_sent = 0;
    }
}

static bool extract_message(std::string& buf, std::string& msg) {
	size_t pos = buf.find('\n');
	if (pos == std::string::npos)
		return false;

	msg = buf.substr(0, pos);
	buf.erase(0, pos + 1);
	return true;
}

struct chat_client *
chat_client_new(std::string_view name)
{
	auto *client = new chat_client();
#if NEED_AUTHOR
    client->name = std::string(name);
    client->name_sent = false;
#endif
    return client;
}

void
chat_client_delete(struct chat_client *client)
{
	if (client->socket >= 0)
		close(client->socket);

	for (auto *m : client->messages)
		delete m;

	delete client;
}

int
chat_client_connect(struct chat_client *client, std::string_view addr)
{
	if (client->socket >= 0)
		return CHAT_ERR_ALREADY_STARTED;

	std::string s(addr);
	auto pos = s.find(':');
	if (pos == std::string::npos)
		return CHAT_ERR_NO_ADDR;

	std::string host = s.substr(0, pos);
	std::string port = s.substr(pos + 1);

	struct addrinfo hints{};
	struct addrinfo *res = nullptr;

	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_STREAM;

	if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0)
		return CHAT_ERR_NO_ADDR;

	int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (sock < 0) {
		freeaddrinfo(res);
		return CHAT_ERR_SYS;
	}

	if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
		freeaddrinfo(res);
		close(sock);
		return CHAT_ERR_SYS;
	}

	fcntl(sock, F_SETFL, O_NONBLOCK);

	client->socket = sock;

	freeaddrinfo(res);
	return 0;
}

struct chat_message *
chat_client_pop_next(struct chat_client *client)
{
	if (client->messages.empty())
		return NULL;

	auto *msg = client->messages.front();
	client->messages.pop_front();
	return msg;
}

int chat_client_update(struct chat_client *client, double timeout)
{
    if (!client || client->socket < 0)
        return CHAT_ERR_NOT_STARTED;

    struct pollfd p{};
    p.fd = client->socket;
    p.events = chat_events_to_poll_events(chat_client_get_events(client));

    int timeout_ms = (timeout < 0) ? -1 : (int)(timeout * 1000);

    int rc = poll(&p, 1, timeout_ms);
    if (rc < 0)
        return CHAT_ERR_SYS;
    if (rc == 0)
        return CHAT_ERR_TIMEOUT;

    if (p.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        close(client->socket);
        client->socket = -1;
        return CHAT_ERR_SYS;
    }

    if (p.revents & POLLIN) {
        char buf[1024];
        while (true) {
            ssize_t n = recv(client->socket, buf, sizeof(buf), 0);
            if (n > 0) {
                client->input_buffer.append(buf, (size_t)n);
            } else if (n == 0) {
                close(client->socket);
                client->socket = -1;
                return CHAT_ERR_SYS;
            } else if (errno == EINTR) {
                continue;
            } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
                break;
            } else {
                close(client->socket);
                client->socket = -1;
                return CHAT_ERR_SYS;
            }
        }

        std::string msg;
        while (extract_message(client->input_buffer, msg)) {
            msg = trim(msg);
            if (msg.empty())
                continue;

            auto *m = new chat_message();

#if NEED_AUTHOR
            size_t pos = msg.find(": ");
            if (pos != std::string::npos) {
                m->author = msg.substr(0, pos);
                m->data = msg.substr(pos + 2);
            } else {
                m->data = msg;
            }
#else
            m->data = msg;
#endif

            client->messages.push_back(m);
        }
    }

    if (p.revents & POLLOUT) {
        flush_output(client);
    }

    if (client->socket >= 0 && !client->output_buffer.empty()) {
        flush_output(client);
    }

    return 0;
}

int
chat_client_get_descriptor(const struct chat_client *client)
{
	return client->socket;
}

int
chat_client_get_events(const struct chat_client *client)
{
	if (!client || client->socket < 0)
		return 0;

	int events = 0; 
	if (client->socket >= 0)
		events |= CHAT_EVENT_INPUT; 

	if (client->output_sent < client->output_buffer.size())
		events |= CHAT_EVENT_OUTPUT;

	return events;
}

int chat_client_feed(struct chat_client *client, const char *msg, uint32_t msg_size)
{
    if (!client || client->socket < 0)
        return CHAT_ERR_NOT_STARTED;
    if (!msg || msg_size == 0)
        return 0;

#if NEED_AUTHOR
    if (!client->name_sent && !client->name.empty()) {
        client->output_buffer.append(client->name);
        client->output_buffer.push_back('\n');
        client->name_sent = true;
    }
#endif

    client->output_buffer.append(msg, msg_size);
    flush_output(client);

    if (client->socket < 0)
        return CHAT_ERR_SYS;

    return 0;
}


