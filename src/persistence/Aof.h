#pragma once
#include <string>
#include <vector>
#include <cstdio>
using namespace std;

// Append-Only File: a log of every write command, in the order it happened.
// On restart we replay this log to rebuild the store. This is the same idea
// as a database's write-ahead log.
class Aof {
public:
    Aof(const string& filePath);
    ~Aof();

    // open the file so new writes can be appended to it
    void openForAppend();

    // log one write command, then force it to physical disk before returning
    // ("always fsync" durability - if the caller continues, it's safely saved)
    void append(const vector<string>& commandArgs);

    // read back every command in the file (used once, at startup, for replay)
    vector<vector<string>> readAllCommands();

    // empty the file (called right after a snapshot captures everything)
    void reset();

private:
    string filePath;
    FILE* fileHandle;
};
