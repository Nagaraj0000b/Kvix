#include "net/Server.h"

// Entry point: build the server on port 6380 and run it.
// All the real work lives in the layers under src/ - this file just starts it.
int main() {
    Server server(6380);
    server.run();
    return 0;
}
