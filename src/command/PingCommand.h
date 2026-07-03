#pragma once
#include "Command.h"
#include "../protocol/RespReply.h"

// PING -> +PONG. Used to check the server is alive.
class PingCommand : public Command {
public:
    string run(CommandContext& context, const vector<string>& args) override {
        return RespReply::simpleString("PONG");
    }
    bool changesData() const override { return false; }
};
