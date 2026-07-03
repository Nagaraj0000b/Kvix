#pragma once
#include <string>
#include <vector>
#include <unordered_map>
#include "../store/DataStore.h"
#include "../persistence/Aof.h"
#include "../persistence/Snapshot.h"
#include "../command/CommandRegistry.h"
using namespace std;

// The network layer: sets up the listening socket, runs the epoll event loop,
// and ties together the store, persistence, and commands. One thread handles
// every client (like real Redis).
class Server {
public:
    Server(int port);

    // recover any saved data from disk, then run the event loop forever
    void run();

private:
    int port;
    int listeningSocket;
    int epollInstance;

    DataStore store;
    Aof aof;
    Snapshot snapshot;
    CommandRegistry registry;

    // Bytes received from each client that haven't formed a full command yet.
    // Keyed by the client's socket. Survives across reads, since one command
    // can arrive split over several read() calls.
    unordered_map<int, string> clientBuffers;

    void recoverFromDisk();
    void setupListeningSocket();
    void acceptNewClients();
    void handleClientData(int clientSocket);
    void closeClient(int clientSocket);

    // look up and run one parsed command, and log it to the AOF if it's a write
    string processCommand(const vector<string>& commandArgs);

    static bool makeSocketNonBlocking(int socketToChange);
};
