
#include "chat.h"
#include "chat_server.h"

#include <vector>
#include <deque>
#include <string>
#include <sys/epoll.h>
#include <fcntl.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <cctype>



struct chat_peer {
	int socket = -1;
    std::string input_buffer;
    std::deque<chat_message*> messages;

    std::string output_buffer;
    size_t output_sent = 0;
#if NEED_AUTHOR
    std::string name; 
    bool name_received = false;
#endif
};

struct chat_server {
	int socket = -1;
    int epoll_fd = -1;

    std::vector<chat_peer*> peers;
    std::deque<chat_message*> messages;

    std::string input_buffer;
};

static void close_peer(struct chat_server *server, chat_peer *peer)
{
    if (!server || !peer || peer->socket < 0)
        return;

    epoll_ctl(server->epoll_fd, EPOLL_CTL_DEL, peer->socket, nullptr);
    close(peer->socket);
    peer->socket = -1;
}

static bool flush_peer_output(struct chat_server *server, chat_peer *peer)
{
    if (!peer || peer->socket < 0)
        return false;

    while (peer->output_sent < peer->output_buffer.size()) {
        ssize_t n = send(
            peer->socket,
            peer->output_buffer.data() + peer->output_sent,
            peer->output_buffer.size() - peer->output_sent,
            0
        );

        if (n > 0) {
            peer->output_sent += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
            return true;
        } else {
            close_peer(server, peer);
            return false;
        }
    }

    if (peer->output_sent == peer->output_buffer.size()) {
        peer->output_buffer.clear();
        peer->output_sent = 0;
    }

    return true;
}

static int accept_pending_clients(struct chat_server *server)
{
    if (!server || server->socket < 0 || server->epoll_fd < 0)
        return CHAT_ERR_NOT_STARTED;

    while (true) {
        struct sockaddr_in cli_addr{};
        socklen_t cli_len = sizeof(cli_addr);

        int cli_sock = accept(server->socket,
                              (struct sockaddr *)&cli_addr,
                              &cli_len);

        if (cli_sock < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return CHAT_ERR_SYS;
        }

        int flags = fcntl(cli_sock, F_GETFL, 0);
        if (flags < 0)
            flags = 0;
        if (fcntl(cli_sock, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(cli_sock);
            return CHAT_ERR_SYS;
        }

        auto *peer = new chat_peer();
        peer->socket = cli_sock;
#if NEED_AUTHOR
        peer->name_received = false;
#endif

        struct epoll_event ev{};
        ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
        ev.data.ptr = peer;

        if (epoll_ctl(server->epoll_fd, EPOLL_CTL_ADD, cli_sock, &ev) < 0) {
            close(cli_sock);
            delete peer;
            return CHAT_ERR_SYS;
        }

        server->peers.push_back(peer);
    }

    return 0;
}

static std::string 
trim(const std::string& s)
{
	size_t l = 0;
	while (l < s.size() && isspace((unsigned char)s[l]))
		l++;

	size_t r = s.size();
	while (r > l && isspace((unsigned char)s[r - 1]))
		r--;

	return s.substr(l, r - l);
}

static bool 
extract_message(std::string& buf, std::string& msg)
{
	size_t pos = buf.find('\n');
	if (pos == std::string::npos)
		return false;

	msg = buf.substr(0, pos);
	buf.erase(0, pos + 1);
	return true;
}



struct chat_server *
chat_server_new(void)
{
    chat_server* server = new chat_server();
    server->socket = -1;
    server->epoll_fd = -1;
    server->peers.clear();
    server->messages.clear();
    server->input_buffer.clear();
    return server;
}

void 
chat_server_delete(struct chat_server *server)
{
    if (!server)
        return;

    if (server->socket >= 0) {
        close(server->socket);
        server->socket = -1;
    }

    if (server->epoll_fd >= 0) {
        close(server->epoll_fd);
        server->epoll_fd = -1;
    }

    for (auto *peer : server->peers) {
        if (!peer) continue;

        if (peer->socket >= 0) {
            close(peer->socket);
            peer->socket = -1;
        }

        for (auto *m : peer->messages) {
            delete m;
        }
        peer->messages.clear();

        delete peer;
    }
    server->peers.clear();

    for (auto *m : server->messages) {
        delete m;
    }
    server->messages.clear();

    delete server;
}

int 
chat_server_listen(struct chat_server *server, uint16_t port)
{
    if (!server)
        return CHAT_ERR_SYS;

    if (server->socket >= 0)
        return CHAT_ERR_ALREADY_STARTED;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return CHAT_ERR_SYS;

    int opt = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        close(sock);
        return CHAT_ERR_SYS;
    }

    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) flags = 0;
    if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        return CHAT_ERR_SYS;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return CHAT_ERR_PORT_BUSY;
    }

    if (listen(sock, SOMAXCONN) < 0) {
        close(sock);
        return CHAT_ERR_SYS;
    }

    int efd = epoll_create1(0);
    if (efd < 0) {
        close(sock);
        return CHAT_ERR_SYS;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = server;
    if (epoll_ctl(efd, EPOLL_CTL_ADD, sock, &ev) < 0) {
        close(sock);
        close(efd);
        return CHAT_ERR_SYS;
    }

    server->socket = sock;
    server->epoll_fd = efd;

    return 0;
}

struct chat_message* 
chat_server_pop_next(struct chat_server *server)
{
    if (!server || server->messages.empty())
        return nullptr;

    chat_message* msg = server->messages.front();
    server->messages.pop_front();
    return msg;
}

int chat_server_update(struct chat_server *server, double timeout)
{
    if (!server || server->socket < 0 || server->epoll_fd < 0)
        return CHAT_ERR_NOT_STARTED;

    struct epoll_event events[16];
    int timeout_ms = (timeout < 0) ? -1 : (int)(timeout * 1000);

    int n = epoll_wait(server->epoll_fd, events, 16, timeout_ms);
    if (n < 0) return CHAT_ERR_SYS;
    if (n == 0) return CHAT_ERR_TIMEOUT;

    bool progress = false;

    for (int i = 0; i < n; ++i) {
        auto *ptr = events[i].data.ptr;

        if (ptr == server) {
            int rc = accept_pending_clients(server);
            if (rc != 0)
                return rc;
            progress = true;
            continue;
        }

        auto *peer = static_cast<chat_peer*>(ptr);
        if (!peer || peer->socket < 0)
            continue;

        if (events[i].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
            close_peer(server, peer);
            progress = true;
            continue;
        }

        if (events[i].events & EPOLLIN) {
            char buf[1024];
            while (true) {
                ssize_t nread = recv(peer->socket, buf, sizeof(buf), 0);
                if (nread > 0) {
                    peer->input_buffer.append(buf, nread);
                    progress = true;
                } else if (nread == 0) {
                    close_peer(server, peer);
                    progress = true;
                    break;
                } else if (errno == EWOULDBLOCK || errno == EAGAIN) {
                    break;
                } else if (errno == EINTR) {
                    continue;
                } else {
                    return CHAT_ERR_SYS;
                }
            }

            while (true) {
                std::string msg;
                if (!extract_message(peer->input_buffer, msg))
                    break;

                msg = trim(msg);
                if (msg.empty())
                    continue;

#if NEED_AUTHOR
                if (!peer->name_received) {
                    peer->name = msg;
                    peer->name_received = true;
                    continue;
                }
#endif

                auto *m = new chat_message();
                m->data = msg;
#if NEED_AUTHOR
                m->author = peer->name;
#endif
                server->messages.push_back(m);
                progress = true;

                for (auto *p : server->peers) {
                    if (!p || p->socket < 0 || p == peer)
                        continue;

#if NEED_AUTHOR
                    std::string full_msg = (peer->name_received && !peer->name.empty())
                                           ? peer->name + ": " + msg + "\n"
                                           : msg + "\n";
#else
                    std::string full_msg = msg + "\n";
#endif

                    p->output_buffer += full_msg;
                    flush_peer_output(server, p);
                    progress = true;
                }
            }
        }

        if ((events[i].events & EPOLLOUT) && peer->output_sent < peer->output_buffer.size()) {
            size_t before = peer->output_sent;
            flush_peer_output(server, peer);
            if (peer->output_sent != before || peer->output_buffer.empty())
                progress = true;
        }
    }

    return progress ? 0 : CHAT_ERR_TIMEOUT;
}

int
chat_server_get_descriptor(const struct chat_server *server)
{
#if NEED_SERVER_FEED
	if (!server || server->epoll_fd < 0)
		return -1;
	return server->epoll_fd;
#else
	(void)server;
	return -1;
#endif
}

int
chat_server_get_socket(const struct chat_server *server)
{
	return server->socket;
}

int
chat_server_get_events(const struct chat_server *server)
{
	if (!server || server->socket < 0)
		return 0;

	int events = CHAT_EVENT_INPUT;

	for (auto *peer : server->peers) {
		if (peer->socket >= 0 &&
		    peer->output_sent < peer->output_buffer.size()) {
			events |= CHAT_EVENT_OUTPUT;
			break;
		}
	}

	return events;
}

int
chat_server_feed(struct chat_server *server, const char *msg, uint32_t msg_size)
{
#if NEED_SERVER_FEED
    if (!server || server->socket < 0)
        return CHAT_ERR_NOT_STARTED;
    if (!msg || msg_size == 0)
        return 0;

    int rc = accept_pending_clients(server);
    if (rc != 0)
        return rc;

    server->input_buffer.append(msg, msg_size);

    std::string line;
    while (extract_message(server->input_buffer, line)) {
        line = trim(line);
        if (line.empty())
            continue;

        auto *m = new chat_message();
#if NEED_AUTHOR
        m->author = "server";
#endif
        m->data = line;
        server->messages.push_back(m);

        for (auto *peer : server->peers) {
            if (!peer || peer->socket < 0)
                continue;

#if NEED_AUTHOR
            peer->output_buffer += "server: " + line + "\n";
#else
            peer->output_buffer += line + "\n";
#endif
            flush_peer_output(server, peer);
        }
    }

    return 0;
#else
    (void)server;
    (void)msg;
    (void)msg_size;
    return CHAT_ERR_NOT_IMPLEMENTED;
#endif
}