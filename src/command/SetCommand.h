#pragma once
#include "Command.h"
#include "../protocol/RespReply.h"

// SET key value -> stores the value, replies +OK.
class SetCommand : public Command {
public:
    string run(CommandContext& context, const vector<string>& args) override {
        if (args.size() != 3) {
            return RespReply::error("wrong number of arguments for 'set' command");
        }
        const string& key = args[1];
        const string& value = args[2];
        context.store.setValue(key, value);
        return RespReply::simpleString("OK");
    }
    bool changesData() const override { return true; }
};
