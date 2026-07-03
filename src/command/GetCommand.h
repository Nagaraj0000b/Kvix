#pragma once
#include "Command.h"
#include "../protocol/RespReply.h"

// GET key -> the value, or nil if the key is missing/expired.
class GetCommand : public Command {
public:
    string run(CommandContext& context, const vector<string>& args) override {
        if (args.size() != 2) {
            return RespReply::error("wrong number of arguments for 'get' command");
        }
        string value;
        if (context.store.getValue(args[1], value)) {
            return RespReply::bulkString(value);
        }
        return RespReply::nil();
    }
    bool changesData() const override { return false; }
};
