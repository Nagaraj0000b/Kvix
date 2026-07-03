#pragma once
#include "Command.h"
#include "../protocol/RespReply.h"

// KEYS pattern -> an array of all keys. Simplification: we only support "*"
// (everything); real glob matching like "user:*" is out of scope. The pattern
// argument is required but ignored.
class KeysCommand : public Command {
public:
    string run(CommandContext& context, const vector<string>& args) override {
        if (args.size() != 2) {
            return RespReply::error("wrong number of arguments for 'keys' command");
        }
        vector<string> allKeys = context.store.getAllKeys();
        return RespReply::array(allKeys);
    }
    bool changesData() const override { return false; }
};
