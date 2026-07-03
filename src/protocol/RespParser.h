#pragma once
#include <string>
#include <vector>
using namespace std;

// Result of trying to read one command out of a buffer of bytes.
// Plain ints instead of an enum - simpler to read, same idea.
const int PARSE_INCOMPLETE = 0;     // not enough bytes yet, wait for more
const int PARSE_COMPLETE = 1;       // got a full command
const int PARSE_PROTOCOL_ERROR = 2; // bytes don't look like valid RESP

struct ParseResult {
    int status;
    vector<string> args;  // filled only when status == PARSE_COMPLETE
    size_t bytesConsumed; // how many bytes to erase from the front of the buffer
};

// Reads ONE complete RESP command (an array of bulk strings) from the front
// of `buffer`, if a whole one is present. Used for both the network wire and
// reading command records back off disk.
ParseResult tryParseCommand(const string& buffer);

// The reverse of tryParseCommand: turns a command's args back into the same
// RESP array-of-bulk-strings bytes. Used to write to the AOF and snapshot.
string encodeCommand(const vector<string>& args);
