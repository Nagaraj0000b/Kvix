#pragma once
#include "Command.h"
#include "../protocol/RespReply.h"

// EXPIRE key seconds -> :1 if the TTL was set, :0 if the key doesn't exist.
class ExpireCommand : public Command {
public:
    string run(CommandContext& context, const vector<string>& args) override {
        if (args.size() != 3) {
            return RespReply::error("wrong number of arguments for 'expire' command");
        }
        long long seconds = stoll(args[2]);
        bool wasSet = context.store.setExpiry(args[1], seconds);
        return RespReply::integer(wasSet ? 1 : 0);
    }
    bool changesData() const override { return true; }
};
