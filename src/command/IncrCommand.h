#pragma once
#include "Command.h"
#include "../protocol/RespReply.h"

// INCR key -> add 1 to the value and reply with the new number. Errors if the
// current value is not a valid integer. A missing key starts at 0.
class IncrCommand : public Command {
public:
    string run(CommandContext& context, const vector<string>& args) override {
        if (args.size() != 2) {
            return RespReply::error("wrong number of arguments for 'incr' command");
        }
        long long newValue;
        if (context.store.incrementValue(args[1], newValue)) {
            return RespReply::integer(newValue);
        }
        return RespReply::error("value is not an integer or out of range");
    }
    bool changesData() const override { return true; }
};
