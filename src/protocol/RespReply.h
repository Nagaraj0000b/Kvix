#pragma once
#include <string>
#include <vector>
using namespace std;

// Small helpers that build the RESP bytes each command replies with.
// Using these keeps the command code readable - e.g. RespReply::integer(1)
// instead of hand-writing ":1\r\n" everywhere.
namespace RespReply {

    // simple string, e.g. +OK\r\n
    inline string simpleString(const string& text) {
        return "+" + text + "\r\n";
    }

    // error, e.g. -ERR something went wrong\r\n
    inline string error(const string& message) {
        return "-ERR " + message + "\r\n";
    }

    // integer, e.g. :42\r\n
    inline string integer(long long number) {
        return ":" + to_string(number) + "\r\n";
    }

    // bulk string, e.g. $5\r\nhello\r\n
    inline string bulkString(const string& text) {
        return "$" + to_string(text.size()) + "\r\n" + text + "\r\n";
    }

    // nil / "key not found", i.e. $-1\r\n  (different from an empty string!)
    inline string nil() {
        return "$-1\r\n";
    }

    // array of bulk strings, e.g. *2\r\n$1\r\na\r\n$1\r\nb\r\n
    inline string array(const vector<string>& items) {
        string out = "*" + to_string(items.size()) + "\r\n";
        for (size_t i = 0; i < items.size(); i++) {
            out += bulkString(items[i]);
        }
        return out;
    }
}
