#pragma once
#include <string>
#include <unordered_map>
#include "Command.h"
using namespace std;

// Holds one object per known command and looks them up by name. This is the
// only place that knows the full list of commands - adding a command means
// adding one registerCommand() line in the constructor, nothing else changes.
class CommandRegistry {
public:
    CommandRegistry();  // creates and registers every command
    ~CommandRegistry(); // deletes them all

    // find a command by name (case-insensitive). returns nullptr if unknown.
    Command* findCommand(const string& name);

private:
    // maps "SET" -> the single SetCommand object, and so on
    unordered_map<string, Command*> commandsByName;

    void registerCommand(const string& name, Command* command);
};
