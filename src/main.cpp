#include "main.hpp"
#include <iostream>
#include <coroutine>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <vector>

static constexpr uint32_t EVENT_READ = EPOLLIN;
static constexpr uint32_t EVENT_WRITE = EPOLLOUT;
static constexpr uint32_t EVENT_ERR = EPOLLERR | EPOLLHUP | EPOLLRDHUP;
static constexpr uint32_t EVENT_RWE = EVENT_READ | EVENT_WRITE | EVENT_ERR;
static constexpr uint32_t EVENT_RE = EVENT_READ | EVENT_ERR;
static constexpr uint32_t EVENT_WE = EVENT_WRITE | EVENT_ERR;

class CoroutineServer
{
public:
    struct promise_type
    {
        CoroutineServer get_return_object()
        {
            return CoroutineServer{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend()
        {
            return {};
        }
        std::suspend_always final_suspend() noexcept
        {
            return {};
        }
        void return_void()
        {
        }
        void unhandled_exception()
        {
            std::cout << "unhandled_exception" << std::endl;
            std::terminate();
        }
        int server_fd{-1};
        int epoll_fd{-1};
    };

    CoroutineServer(std::coroutine_handle<promise_type> h) : coro_handle(h)
    {
    }
    ~CoroutineServer()
    {
    }
    std::coroutine_handle<promise_type> coro_handle;
};

class CoroutineClientConn
{
public:
    struct promise_type
    {
        CoroutineClientConn get_return_object()
        {
            return CoroutineClientConn{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception()
        {
            std::cout << "unhandled_exception" << std::endl;
            std::terminate();
        }

        uint32_t event_come{0};
    };

    CoroutineClientConn(std::coroutine_handle<promise_type> h) : coro_handle(h) {}
    ~CoroutineClientConn()
    {
        std::cout << "~CoroutineClientConn" << std::endl;
        // if (coro_handle)
        //     coro_handle.destroy();
    }

    std::coroutine_handle<promise_type> coro_handle;
};

class SocketAwaiter
{
public:
    SocketAwaiter(int fd, uint32_t events, int epoll_fd) : fd(fd),
                                                           events(events),
                                                           epoll_fd(epoll_fd)
    {
    }

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<CoroutineClientConn::promise_type> h)
    {
        coro_handle = h;
        epoll_event event;
        event.data.ptr = static_cast<void *>(coro_handle.address());
        event.events = events | EPOLLONESHOT;
        epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &event);
    }
    uint32_t await_resume() noexcept
    {
        return coro_handle.promise().event_come;
    }

private:
    int fd;
    uint32_t events;
    int epoll_fd;
    std::coroutine_handle<CoroutineClientConn::promise_type> coro_handle;
};

void set_non_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

CoroutineClientConn CreateCoroutineClientConn(int client_socket, int epoll_fd)
{
    std::cout << "handle_client(" << client_socket << ")" << std::endl;

    char buffer[1024];
    std::string send_buffer;

    auto on_co_return = [&client_socket, &epoll_fd]() -> void
    {
        if (client_socket > 0)
        {
            close(client_socket);
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_socket, nullptr);
        }
    };

    while (true)
    {
        // 等待可读事件
        SocketAwaiter read_awaiter{client_socket, EVENT_RE, epoll_fd};
        uint32_t event_come = co_await read_awaiter;
        if (event_come & EVENT_ERR)
        {
            std::cout << "client_socket " << client_socket << " event_come " << event_come << std::endl;
            on_co_return();
            co_return;
        }

        int bytes_read = recv(client_socket, buffer, sizeof(buffer), 0);
        if (bytes_read == 0)
        {
            on_co_return();
            co_return;
        }

        if (bytes_read < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            {
                on_co_return();
                co_return;
            }
        }

        // 将收到的数据加入发送缓冲区
        if (bytes_read > 0)
        {
            std::cout << "bytes_read " << bytes_read << std::endl;
            send_buffer.append(buffer, bytes_read);
        }

        // 当有数据需要发送时，进入发送逻辑
        while (!send_buffer.empty())
        {
            // 等待可写事件
            SocketAwaiter write_awaiter{client_socket, EVENT_WE, epoll_fd};
            event_come = co_await write_awaiter;

            if (event_come & EVENT_ERR)
            {
                on_co_return();
                co_return;
            }

            int bytes_sent = send(client_socket, send_buffer.c_str(), send_buffer.size(), 0);

            if (bytes_sent == 0)
            {
                std::cerr << "Send error" << std::endl;
                on_co_return();
                co_return;
            }

            if (bytes_sent < 0)
            {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                {
                    on_co_return();
                    co_return;
                }
                else
                {
                    continue;
                }
            }

            // 删除已经发送的数据
            if (bytes_sent > 0)
            {
                send_buffer.erase(0, bytes_sent);
            }
        }
    }
}

CoroutineServer CreateCoroutineServer(const int PORT, const int MAX_EVENTS)
{
    int server_fd = -1;
    int epoll_fd = -1;

    auto on_co_return = [&server_fd, &epoll_fd]() -> void
    {
        if (server_fd > 0)
        {
            close(server_fd);
        }
        if (epoll_fd > 0)
        {
            close(epoll_fd);
        }
    };

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd <= 0)
    {
        std::cerr << "Socket creation failed\n";
        on_co_return();
        co_return;
    }

    set_non_blocking(server_fd);

    sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(PORT);

    if (bind(server_fd, (sockaddr *)&address, sizeof(address)) < 0)
    {
        std::cerr << "Bind failed\n";
        on_co_return();
        co_return;
    }

    if (listen(server_fd, 3) < 0)
    {
        std::cerr << "Listen failed\n";
        on_co_return();
        co_return;
    }

    epoll_fd = epoll_create1(0);
    if (epoll_fd == -1)
    {
        std::cerr << "Epoll creation failed\n";
        on_co_return();
        co_return;
    }

    epoll_event event;
    event.data.fd = server_fd;
    event.events = EVENT_RE;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, server_fd, &event);

    std::vector<epoll_event> events(MAX_EVENTS);

    while (true)
    {
        int n = epoll_wait(epoll_fd, events.data(), MAX_EVENTS, -1);
        for (int i = 0; i < n; i++)
        {
            if (events[i].data.fd == server_fd)
            {
                if (events[i].events & EVENT_ERR)
                {
                    std::cout << "server fd event " << events[i].events << std::endl;
                    std::terminate();
                }
                int client_socket = accept(server_fd, nullptr, nullptr);
                if (client_socket >= 0)
                {
                    std::cout << "new client_socket " << client_socket << std::endl;
                    set_non_blocking(client_socket);
                    epoll_event client_event;
                    client_event.data.fd = client_socket;
                    client_event.events = EVENT_RE;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_socket, &client_event);

                    // Client Connection Coroutine
                    CoroutineClientConn coroutineClientConn = CreateCoroutineClientConn(client_socket, epoll_fd);
                    coroutineClientConn.coro_handle.promise().event_come = EVENT_READ;
                    coroutineClientConn.coro_handle.resume();
                }
                std::cout << "coroutineClientConn.coro_handle.resume() over" << std::endl;
            }
            else
            {
                std::cout << "event client " << reinterpret_cast<uint64_t>(events[i].data.ptr) << std::endl;
                std::coroutine_handle<CoroutineClientConn::promise_type> coroutineClientConnHandle = std::coroutine_handle<CoroutineClientConn::promise_type>::from_address(events[i].data.ptr);
                if (coroutineClientConnHandle.done())
                {
                    std::cout << "coroutineClientConnHandle done" << std::endl;
                }
                else
                {
                    coroutineClientConnHandle.promise().event_come = events[i].events;
                    coroutineClientConnHandle.resume();
                }
                if (coroutineClientConnHandle.done())
                {
                    std::cout << "coroutineClientConnHandle done" << std::endl;
                    coroutineClientConnHandle.destroy();
                }
            }
        }
    }

    on_co_return();
    co_return;
}

int main(int argc, char **argv)
{
    // Server Coroutine
    CoroutineServer server = CreateCoroutineServer(20023, 100000);
    server.coro_handle.resume();
    if (server.coro_handle.done())
    {
        server.coro_handle.destroy();
    }
    return 0;
}