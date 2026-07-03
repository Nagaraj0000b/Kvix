#pragma once
#include "Command.h"
#include "../protocol/RespReply.h"

// EXISTS key -> :1 if the key exists (and isn't expired), else :0.
class ExistsCommand : public Command {
public:
    string run(CommandContext& context, const vector<string>& args) override {
        if (args.size() != 2) {
            return RespReply::error("wrong number of arguments for 'exists' command");
        }
        bool present = context.store.hasKey(args[1]);
        return RespReply::integer(present ? 1 : 0);
    }
    bool changesData() const override { return false; }
};
