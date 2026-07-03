#pragma once
#include "Command.h"
#include "../protocol/RespReply.h"

// FLUSHALL -> empty the whole store, reply +OK.
class FlushAllCommand : public Command {
public:
    string run(CommandContext& context, const vector<string>& args) override {
        context.store.clearAll();
        return RespReply::simpleString("OK");
    }
    bool changesData() const override { return true; }
};
