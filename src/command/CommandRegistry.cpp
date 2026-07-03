#include "CommandRegistry.h"
#include <cctype> // toupper

// One include per command - this is the single file that knows them all.
#include "PingCommand.h"
#include "SetCommand.h"
#include "GetCommand.h"
#include "DelCommand.h"
#include "ExpireCommand.h"
#include "TtlCommand.h"
#include "ExistsCommand.h"
#include "IncrCommand.h"
#include "KeysCommand.h"
#include "FlushAllCommand.h"
#include "SaveCommand.h"

// Uppercase a copy of the text, so "set", "SET" and "Set" all match.
static string toUpperCase(const string& text) {
    string result = text;
    for (size_t i = 0; i < result.size(); i++) {
        result[i] = toupper((unsigned char)result[i]);
    }
    return result;
}

CommandRegistry::CommandRegistry() {
    // Register every command once, by name. To add a new command, write its
    // class in a new header and add one line here - nothing else changes.
    registerCommand("PING", new PingCommand());
    registerCommand("SET", new SetCommand());
    registerCommand("GET", new GetCommand());
    registerCommand("DEL", new DelCommand());
    registerCommand("EXPIRE", new ExpireCommand());
    registerCommand("TTL", new TtlCommand());
    registerCommand("EXISTS", new ExistsCommand());
    registerCommand("INCR", new IncrCommand());
    registerCommand("KEYS", new KeysCommand());
    registerCommand("FLUSHALL", new FlushAllCommand());
    registerCommand("SAVE", new SaveCommand());
}

CommandRegistry::~CommandRegistry() {
    // we created each command with `new`, so we delete each one here
    for (unordered_map<string, Command*>::iterator it = commandsByName.begin();
         it != commandsByName.end(); ++it) {
        delete it->second;
    }
}

void CommandRegistry::registerCommand(const string& name, Command* command) {
    commandsByName[name] = command;
}

Command* CommandRegistry::findCommand(const string& name) {
    string upperName = toUpperCase(name);
    unordered_map<string, Command*>::iterator found = commandsByName.find(upperName);
    if (found == commandsByName.end()) {
        return nullptr;
    }
    return found->second;
}
