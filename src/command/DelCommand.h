#pragma once
#include "Command.h"
#include "../protocol/RespReply.h"

// DEL key -> :1 if the key was removed, :0 if it wasn't there.
class DelCommand : public Command {
public:
    string run(CommandContext& context, const vector<string>& args) override {
        if (args.size() != 2) {
            return RespReply::error("wrong number of arguments for 'del' command");
        }
        bool removed = context.store.deleteKey(args[1]);
        return RespReply::integer(removed ? 1 : 0);
    }
    bool changesData() const override { return true; }
};
