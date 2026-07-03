#pragma once
#include "Command.h"
#include "../protocol/RespReply.h"

// SAVE -> dump the whole store to a snapshot file, then empty the AOF (the
// snapshot now covers everything, so the old log is no longer needed).
// Not itself logged to the AOF - that's why changesData() is false.
class SaveCommand : public Command {
public:
    string run(CommandContext& context, const vector<string>& args) override {
        context.snapshot.save(context.store);
        context.aof.reset();
        return RespReply::simpleString("OK");
    }
    bool changesData() const override { return false; }
};
