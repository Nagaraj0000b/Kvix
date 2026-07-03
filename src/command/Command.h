#pragma once
#include <string>
#include <vector>
#include "../store/DataStore.h"
#include "../persistence/Snapshot.h"
#include "../persistence/Aof.h"
using namespace std;

// Everything a command might need to do its job, bundled together.
// Most commands only use `store`; SAVE also needs snapshot and aof.
struct CommandContext {
    DataStore& store;
    Snapshot& snapshot;
    Aof& aof;
};

// The base class every command inherits from. This is the heart of the SOLID
// design: to add a new command you write a new small class that overrides
// run() and register it once - you never edit the dispatcher or other commands
// (Open/Closed Principle).
class Command {
public:
    virtual ~Command() {}

    // do the work and return the RESP-encoded reply bytes
    virtual string run(CommandContext& context, const vector<string>& args) = 0;

    // true for commands that change stored data (SET, DEL, ...). The server
    // checks this to decide whether the command should be logged to the AOF.
    virtual bool changesData() const = 0;
};
