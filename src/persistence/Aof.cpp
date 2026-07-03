#include "Aof.h"
#include "../protocol/RespParser.h"
#include <fstream>
#include <unistd.h> // fsync, fileno
#include <iterator> // istreambuf_iterator

Aof::Aof(const string& path) {
    filePath = path;
    fileHandle = nullptr;
}

Aof::~Aof() {
    if (fileHandle != nullptr) {
        fclose(fileHandle);
    }
}

void Aof::openForAppend() {
    fileHandle = fopen(filePath.c_str(), "a");
}

void Aof::append(const vector<string>& commandArgs) {
    if (fileHandle == nullptr) {
        return;
    }
    string encoded = encodeCommand(commandArgs);
    fwrite(encoded.data(), 1, encoded.size(), fileHandle);
    fflush(fileHandle);        // push out of our buffer into the OS
    fsync(fileno(fileHandle)); // and force the OS to write to physical disk
}

vector<vector<string>> Aof::readAllCommands() {
    vector<vector<string>> commands;

    ifstream in(filePath, ios::binary);
    if (!in.is_open()) {
        return commands; // no file yet - nothing to replay
    }
    string contents((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
    in.close();

    // The file is just RESP command records back to back - the same shape a
    // client would have sent - so we read it with the exact same parser.
    // (substr each loop makes this O(n^2) in file size; fine for startup.)
    size_t offset = 0;
    while (offset < contents.size()) {
        string remaining = contents.substr(offset);
        ParseResult parsed = tryParseCommand(remaining);
        if (parsed.status != PARSE_COMPLETE) {
            break; // incomplete or corrupt tail - stop here
        }
        commands.push_back(parsed.args);
        offset += parsed.bytesConsumed;
    }
    return commands;
}

void Aof::reset() {
    if (fileHandle != nullptr) {
        fclose(fileHandle);
    }
    fileHandle = fopen(filePath.c_str(), "w"); // "w" truncates the file to empty
    fclose(fileHandle);
    fileHandle = fopen(filePath.c_str(), "a"); // reopen for future appends
}
