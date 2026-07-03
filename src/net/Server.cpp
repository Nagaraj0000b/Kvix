#include "Server.h"
#include "../protocol/RespParser.h"
#include "../protocol/RespReply.h"

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cerrno>

Server::Server(int serverPort)
    : port(serverPort),
      listeningSocket(-1),
      epollInstance(-1),
      aof("cppcache.aof"),
      snapshot("cppcache.snapshot")
{
}

bool Server::makeSocketNonBlocking(int socketToChange) {
    int flags = fcntl(socketToChange, F_GETFL, 0); // read existing flags first
    if (flags == -1) {
        return false;
    }
    // OR in O_NONBLOCK so we don't clobber flags the kernel already set
    return fcntl(socketToChange, F_SETFL, flags | O_NONBLOCK) != -1;
}

void Server::run() {
    recoverFromDisk();
    setupListeningSocket();

    const int MAX_EVENTS = 64;
    epoll_event readyEvents[MAX_EVENTS];

    while (true) {
        // Wait up to 1 second for any socket to become ready. If nothing does,
        // epoll_wait returns 0 and we use that moment for the active expiry
        // sweep - no separate timer or thread needed.
        int readyCount = epoll_wait(epollInstance, readyEvents, MAX_EVENTS, 1000);
        if (readyCount == -1) {
            perror("epoll_wait failed");
            continue;
        }
        if (readyCount == 0) {
            store.removeExpiredKeys(); // active expiration tick
            continue;
        }

        for (int i = 0; i < readyCount; i++) {
            int readySocket = readyEvents[i].data.fd;
            if (readySocket == listeningSocket) {
                acceptNewClients();
            } else {
                handleClientData(readySocket);
            }
        }
    }
}

void Server::recoverFromDisk() {
    // 1) load the last snapshot (fast bulk restore of the base state)
    snapshot.load(store);

    // 2) replay every write recorded after that snapshot, catching the store
    //    up to exactly where it was when the process last stopped. We run them
    //    directly (not through processCommand) so they are NOT re-logged.
    vector<vector<string>> pastCommands = aof.readAllCommands();
    for (size_t i = 0; i < pastCommands.size(); i++) {
        Command* command = registry.findCommand(pastCommands[i][0]);
        if (command != nullptr) {
            CommandContext context { store, snapshot, aof };
            command->run(context, pastCommands[i]);
        }
    }

    // 3) now open the AOF so all future live writes get appended
    aof.openForAppend();
    printf("recovered %zu key(s) from disk\n", store.getAllKeys().size());
}

void Server::setupListeningSocket() {
    listeningSocket = socket(AF_INET, SOCK_STREAM, 0); // IPv4, TCP
    if (listeningSocket == -1) {
        perror("socket failed");
        exit(1);
    }

    int reuse = 1; // lets us restart on the same port without waiting
    setsockopt(listeningSocket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = INADDR_ANY; // all local interfaces
    serverAddress.sin_port = htons(port);

    if (bind(listeningSocket, (sockaddr*)&serverAddress, sizeof(serverAddress)) == -1) {
        perror("bind failed");
        exit(1);
    }
    if (listen(listeningSocket, 5) == -1) {
        perror("listen failed");
        exit(1);
    }
    makeSocketNonBlocking(listeningSocket);

    epollInstance = epoll_create1(0);
    if (epollInstance == -1) {
        perror("epoll_create1 failed");
        exit(1);
    }

    epoll_event listenEvent{};
    listenEvent.events = EPOLLIN;
    listenEvent.data.fd = listeningSocket;
    epoll_ctl(epollInstance, EPOLL_CTL_ADD, listeningSocket, &listenEvent);

    printf("listening on port %d (epoll)...\n", port);
}

void Server::acceptNewClients() {
    // Accept every pending connection, not just one - epoll only told us the
    // listener is "ready", there may be several waiting.
    while (true) {
        sockaddr_in clientAddress{};
        socklen_t addressLength = sizeof(clientAddress);
        int clientSocket = accept(listeningSocket, (sockaddr*)&clientAddress, &addressLength);
        if (clientSocket == -1) {
            // no more pending connections right now - normal, stop
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                perror("accept failed");
            }
            break;
        }

        makeSocketNonBlocking(clientSocket);

        epoll_event clientEvent{};
        clientEvent.events = EPOLLIN;
        clientEvent.data.fd = clientSocket;
        epoll_ctl(epollInstance, EPOLL_CTL_ADD, clientSocket, &clientEvent);

        clientBuffers[clientSocket] = ""; // start this client's buffer empty
        printf("client connected (fd=%d)\n", clientSocket);
    }
}

void Server::handleClientData(int clientSocket) {
    char readBuffer[1024];
    bool shouldClose = false;

    // Read everything available right now (non-blocking, so it stops on EAGAIN).
    while (true) {
        ssize_t bytesRead = read(clientSocket, readBuffer, sizeof(readBuffer));
        if (bytesRead > 0) {
            clientBuffers[clientSocket].append(readBuffer, bytesRead);

            // Pull out and run as many complete commands as the buffer holds.
            while (true) {
                ParseResult parsed = tryParseCommand(clientBuffers[clientSocket]);
                if (parsed.status == PARSE_INCOMPLETE) {
                    break; // wait for more bytes next time
                }
                if (parsed.status == PARSE_PROTOCOL_ERROR) {
                    string errorReply = RespReply::error("protocol error");
                    write(clientSocket, errorReply.c_str(), errorReply.size());
                    shouldClose = true;
                    break;
                }
                clientBuffers[clientSocket].erase(0, parsed.bytesConsumed);
                string reply = processCommand(parsed.args);
                write(clientSocket, reply.c_str(), reply.size());
            }
            if (shouldClose) {
                break;
            }
        } else if (bytesRead == 0) {
            shouldClose = true; // client closed the connection
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // nothing more to read right now - normal
            }
            perror("read failed");
            shouldClose = true;
            break;
        }
    }

    if (shouldClose) {
        closeClient(clientSocket);
    }
}

void Server::closeClient(int clientSocket) {
    // remove from epoll BEFORE close(), because the socket number can be reused
    // by a new connection the instant it's closed
    epoll_ctl(epollInstance, EPOLL_CTL_DEL, clientSocket, nullptr);
    close(clientSocket);
    clientBuffers.erase(clientSocket);
    printf("client disconnected (fd=%d)\n", clientSocket);
}

string Server::processCommand(const vector<string>& commandArgs) {
    Command* command = registry.findCommand(commandArgs[0]);
    if (command == nullptr) {
        return RespReply::error("unknown command '" + commandArgs[0] + "'");
    }

    CommandContext context { store, snapshot, aof };
    string reply = command->run(context, commandArgs);

    // Durability: log write commands to the AOF only if they succeeded (a
    // reply starting with '-' is an error). We log AFTER running so we don't
    // record commands that failed validation.
    bool replyIsError = (!reply.empty() && reply[0] == '-');
    if (command->changesData() && !replyIsError) {
        aof.append(commandArgs);
    }
    return reply;
}
