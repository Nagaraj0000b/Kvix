#pragma once
#include "Command.h"
#include "../protocol/RespReply.h"

// TTL key -> seconds left, or -1 (no TTL set) or -2 (key doesn't exist).
class TtlCommand : public Command {
public:
    string run(CommandContext& context, const vector<string>& args) override {
        if (args.size() != 2) {
            return RespReply::error("wrong number of arguments for 'ttl' command");
        }
        long long secondsLeft = context.store.getTimeToLive(args[1]);
        return RespReply::integer(secondsLeft);
    }
    bool changesData() const override { return false; }
};
