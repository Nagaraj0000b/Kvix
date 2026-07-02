#include <sys/socket.h>
#include <sys/epoll.h> // epoll_create1, epoll_ctl, epoll_wait
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <netinet/in.h>
#include <stdlib.h>
#include <cerrno>
#include <fcntl.h> // fcntl, O_NONBLOCK

// Flip a fd into non-blocking mode.
// Needed for server_fd AND every client_fd, so pulled into a helper
// instead of repeating the fcntl dance each time.
bool setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0); // read EXISTING flags first...
    if (flags == -1)
        return false;
    // ...then OR in O_NONBLOCK instead of overwriting, so we don't
    // clobber other flags the kernel may already have set.
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
}

int main()
{

    int server_fd = socket(AF_INET, SOCK_STREAM, 0); // ipv4 , tcp, default protocol tcp
    if (server_fd == -1)
    {
        perror("socket failed");
        exit(1);
    }
    int opt = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) == -1)
    {
        perror("socketopt failed");
        exit(1);
    }
    sockaddr_in adress{};                // zero-initialize the struct
    adress.sin_family = AF_INET;         // IPv4
    adress.sin_addr.s_addr = INADDR_ANY; // listen on all local interfaces (0.0.0.0)
    adress.sin_port = htons(6380);       // htons = host-to-network byte order (endianness)

    if (bind(server_fd, (sockaddr *)&adress, sizeof(adress)) == -1)
    {
        perror("bind failed");
        exit(1);
    }

    if (listen(server_fd, 5) == -1)
    {
        perror("listen failed");
        exit(1);
    }

    // CHANGED: server_fd is now non-blocking.
    // Reasoning: epoll_wait() saying "server_fd is ready" means a
    // connection *was* pending, but by the time we call accept() it
    // could theoretically be gone (e.g. client disconnected instantly).
    // A blocking server_fd would then freeze the whole loop for every
    // other client. Non-blocking + checking for EAGAIN makes that safe.
    if (!setNonBlocking(server_fd))
    {
        perror("fcntl failed");
        exit(1);
    }

    // CHANGED: create the epoll instance.
    // Reasoning: epoll is a kernel-side watch list. Instead of blocking
    // on one fd at a time (accept() then read()), we ask the kernel once
    // per loop iteration "which of ALL my watched fds are ready?".
    int epfd = epoll_create1(0);
    if (epfd == -1)
    {
        perror("epoll_create1 failed");
        exit(1);
    }

    // CHANGED: register server_fd with epoll so we're notified when a
    // new client wants to connect.
    // Reasoning: EPOLLIN on a *listening* socket means "a connection is
    // waiting in the accept queue", not "data to read".
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = server_fd;
    if (epoll_ctl(epfd, EPOLL_CTL_ADD, server_fd, &ev) == -1)
    {
        perror("epoll_ctl add server_fd failed");
        exit(1);
    }

    printf("listening on port 6380 (epoll)...\n");

    const int MAX_EVENTS = 64;
    epoll_event events[MAX_EVENTS];
    char buf[1024];

    // CHANGED: this replaces the old "accept() then block in an inner
    // read/write loop for that one client" structure entirely.
    // Reasoning: there's no per-client blocking section anymore. Every
    // fd - listening or client - is handled the same way: wait for epoll
    // to say it's ready, handle only that fd, go back to waiting. That's
    // what lets a single thread serve many clients: no one fd can hog it.
    while (true)
    {
        // -1 timeout = block here. This is the ONLY blocking call left,
        // and it blocks on ALL watched fds at once, not just one.
        int n = epoll_wait(epfd, events, MAX_EVENTS, -1);
        if (n == -1)
        {
            perror("epoll_wait failed");
            continue;
        }

        for (int i = 0; i < n; i++)
        {
            int fd = events[i].data.fd;

            if (fd == server_fd)
            {
                // CHANGED: drain ALL pending connections, not just one.
                // Reasoning: epoll_wait only told us "ready" - there could
                // be several queued. Accepting just once per notification
                // would leave others waiting until some unrelated event
                // happened to wake the loop again.
                while (true)
                {
                    sockaddr_in client_addr{};
                    socklen_t client_len = sizeof(client_addr);
                    int client_fd = accept(server_fd, (sockaddr *)&client_addr, &client_len);
                    if (client_fd == -1)
                    {
                        // EAGAIN/EWOULDBLOCK = no more pending connections
                        // right now - normal "done draining" signal, not
                        // an error.
                        if (errno != EAGAIN && errno != EWOULDBLOCK)
                        {
                            perror("accept failed");
                        }
                        break;
                    }

                    setNonBlocking(client_fd);

                    epoll_event cev{};
                    cev.events = EPOLLIN;
                    cev.data.fd = client_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &cev);

                    printf("client connected (fd=%d)\n", client_fd);
                }
            }
            else
            {
                // CHANGED: a specific client fd has data. Handle just
                // this one client, then return to epoll_wait - we do NOT
                // sit here echoing forever like the old inner loop did.
                bool clientClosed = false;

                // Drain this client's data until EAGAIN. Level-triggered
                // epoll would keep re-notifying if we left data unread,
                // but draining now is simpler and cheaper than relying
                // on repeated wakeups.
                while (true)
                {
                    ssize_t r = read(fd, buf, sizeof(buf));
                    if (r > 0)
                    {
                        write(fd, buf, r); // echo back
                    }
                    else if (r == 0)
                    {
                        // client closed the connection
                        printf("client disconnected (fd=%d)\n", fd);
                        clientClosed = true;
                        break;
                    }
                    else
                    { // r == -1
                        if (errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            break; // nothing more to read right now - normal
                        }
                        perror("read failed");
                        clientClosed = true;
                        break;
                    }
                }

                if (clientClosed)
                {
                    // CHANGED: remove from epoll BEFORE close().
                    // Reasoning: once closed, a fd number can be reused
                    // instantly by a brand new connection. Deleting from
                    // epoll after close (or worse, after it's been reused)
                    // risks operating on the wrong fd's epoll registration.
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                }
            }
        }
    }

    close(server_fd);

    return 0;
}
