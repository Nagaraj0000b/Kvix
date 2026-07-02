#include <sys/socket.h>
#include <sys/epoll.h> // epoll_create1, epoll_ctl, epoll_wait
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <netinet/in.h>
#include <stdlib.h>
#include <cerrno>
#include <fcntl.h> // fcntl, O_NONBLOCK
#include <string>
#include <vector>
#include <unordered_map>
#include <cctype> // toupper
using namespace std;

// ---- RESP parser (from test.cpp) ----
// Plain ints instead of an enum - simpler, same idea:
const int PARSE_INCOMPLETE = 0;      // not enough bytes yet, wait for more data
const int PARSE_COMPLETE = 1;        // got a full command
const int PARSE_PROTOCOL_ERROR = 2;  // buffer doesn't look like valid RESP

struct ParseResult {
    int status;
    vector<string> args;   // filled only if status == PARSE_COMPLETE
    size_t bytesConsumed;  // how much to erase from the front of the buffer, if COMPLETE
};

// Given the raw bytes received so far for one connection, try to pull ONE
// complete RESP command (an array of bulk strings) out of the front of it.
ParseResult tryParseCommand(const string& buf) {
    ParseResult result;
    result.status = PARSE_INCOMPLETE;
    result.bytesConsumed = 0;

    if (buf.size() < 1) {
        return result;  // nothing arrived yet
    }
    if (buf[0] != '*') {
        result.status = PARSE_PROTOCOL_ERROR;
        return result;
    }

    // ---- parse "*<argc>\r\n" ----
    size_t lineEnd = buf.find("\r\n", 0);
    if (lineEnd == string::npos) {
        return result;  // haven't seen the end of the first line yet
    }
    string countStr = buf.substr(1, lineEnd - 1);  // between '*' and "\r\n"
    int argc = stoi(countStr);

    size_t pos = lineEnd + 2;  // cursor: move past "*3\r\n"
    vector<string> args;

    // ---- parse each "$<len>\r\n<data>\r\n" element ----
    for (int i = 0; i < argc; i++) {
        if (pos >= buf.size()) {
            return result;  // ran out of buffer before seeing '$'
        }
        if (buf[pos] != '$') {
            result.status = PARSE_PROTOCOL_ERROR;
            return result;
        }

        size_t lenLineEnd = buf.find("\r\n", pos);
        if (lenLineEnd == string::npos) {
            return result;  // length line not finished yet
        }
        string lenStr = buf.substr(pos + 1, lenLineEnd - (pos + 1));
        int len = stoi(lenStr);

        pos = lenLineEnd + 2;  // cursor: move past "$3\r\n"

        // need `len` data bytes plus a trailing "\r\n"
        if (buf.size() < pos + len + 2) {
            return result;  // data not fully arrived yet
        }

        string value = buf.substr(pos, len);
        if (buf[pos + len] != '\r' || buf[pos + len + 1] != '\n') {
            result.status = PARSE_PROTOCOL_ERROR;
            return result;
        }

        args.push_back(value);
        pos = pos + len + 2;  // move past the value and its trailing "\r\n"
    }

    // ---- all argc elements parsed successfully ----
    result.status = PARSE_COMPLETE;
    result.args = args;
    result.bytesConsumed = pos;
    return result;
}

// ---- command dispatch ----
// One global hash map is the whole storage engine for now (Week 2 adds
// TTL/LRU on top of this same idea).
unordered_map<string, string> store;

// Uppercase a copy of a string, so "set"/"SET"/"Set" all dispatch the same way.
string toUpper(const string& s) {
    string out = s;
    for (size_t i = 0; i < out.size(); i++) {
        out[i] = toupper((unsigned char)out[i]);
    }
    return out;
}

// Takes the parsed command (args[0] = command name, rest = arguments) and
// returns the RESP-encoded reply to write back to the client.
string handleCommand(vector<string>& args) {
    string cmd = toUpper(args[0]);

    if (cmd == "PING") {
        return "+PONG\r\n";
    }

    if (cmd == "SET") {
        if (args.size() != 3) {
            return "-ERR wrong number of arguments for 'set' command\r\n";
        }
        store[args[1]] = args[2];
        return "+OK\r\n";
    }

    if (cmd == "GET") {
        if (args.size() != 2) {
            return "-ERR wrong number of arguments for 'get' command\r\n";
        }
        unordered_map<string, string>::iterator it = store.find(args[1]);
        if (it == store.end()) {
            return "$-1\r\n"; // RESP nil - key not found
        }
        string& value = it->second;
        return "$" + to_string(value.size()) + "\r\n" + value + "\r\n";
    }

    if (cmd == "DEL") {
        if (args.size() != 2) {
            return "-ERR wrong number of arguments for 'del' command\r\n";
        }
        size_t removed = store.erase(args[1]);
        return ":" + to_string(removed) + "\r\n";
    }

    return "-ERR unknown command '" + args[0] + "'\r\n";
}

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

    // server_fd is non-blocking so a stale/gone connection can never
    // freeze the whole loop for every other client.
    if (!setNonBlocking(server_fd))
    {
        perror("fcntl failed");
        exit(1);
    }

    // epoll is a kernel-side watch list. Instead of blocking on one fd at
    // a time (accept() then read()), we ask the kernel once per loop
    // iteration "which of ALL my watched fds are ready?".
    int epfd = epoll_create1(0);
    if (epfd == -1)
    {
        perror("epoll_create1 failed");
        exit(1);
    }

    // register server_fd with epoll so we're notified when a new client
    // wants to connect. Ready here means "a connection is waiting in the
    // accept queue", not "data to read".
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
    char readBuf[1024];

    // CHANGED: one buffer per client fd, holding whatever bytes have
    // arrived for that client but haven't formed a complete RESP command
    // yet. Reasoning: a client's command can arrive split across more
    // than one read() - this is the state that survives between reads.
    unordered_map<int, string> clientBuffers;

    // Every fd - listening or client - is handled the same way: wait for
    // epoll to say it's ready, handle only that fd, go back to waiting.
    // That's what lets a single thread serve many clients: no one fd can
    // hog the thread.
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
                // drain ALL pending connections, not just one - epoll_wait
                // only told us "ready", there could be several queued.
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

                    // CHANGED: give this new client an empty buffer to
                    // accumulate into.
                    clientBuffers[client_fd] = "";

                    printf("client connected (fd=%d)\n", client_fd);
                }
            }
            else
            {
                // a specific client fd has data. Handle just this one
                // client, then return to epoll_wait - never sit here
                // serving one client forever.
                bool clientClosed = false;

                // Drain this client's data until EAGAIN.
                while (true)
                {
                    ssize_t r = read(fd, readBuf, sizeof(readBuf));
                    if (r > 0)
                    {
                        // CHANGED: append raw bytes to this client's
                        // buffer instead of echoing them back directly.
                        clientBuffers[fd].append(readBuf, r);

                        // CHANGED: try to pull as many complete commands
                        // out of the buffer as are currently sitting in
                        // it - there could be more than one queued up.
                        while (true)
                        {
                            ParseResult pr = tryParseCommand(clientBuffers[fd]);

                            if (pr.status == PARSE_INCOMPLETE)
                            {
                                break; // wait for more bytes on the next read()
                            }

                            if (pr.status == PARSE_PROTOCOL_ERROR)
                            {
                                string err = "-ERR protocol error\r\n";
                                write(fd, err.c_str(), err.size());
                                clientClosed = true;
                                break;
                            }

                            // PARSE_COMPLETE: consume it and dispatch.
                            clientBuffers[fd].erase(0, pr.bytesConsumed);
                            string reply = handleCommand(pr.args);
                            write(fd, reply.c_str(), reply.size());
                        }

                        if (clientClosed)
                        {
                            break; // stop reading more from this fd
                        }
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
                    // remove from epoll BEFORE close() - once closed, a fd
                    // number can be reused instantly by a brand new
                    // connection, so this order avoids operating on the
                    // wrong fd's epoll registration.
                    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                    // CHANGED: also drop this client's buffer, or it
                    // leaks in clientBuffers forever after disconnect.
                    clientBuffers.erase(fd);
                }
            }
        }
    }

    close(server_fd);

    return 0;
}
