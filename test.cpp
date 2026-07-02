#include <string>
#include <vector>
#include <cstdio>
using namespace std;

// Status codes for tryParseCommand's result.
// Plain ints instead of an enum - simpler, same idea:
const int PARSE_INCOMPLETE = 0;      // not enough bytes yet, wait for more data
const int PARSE_COMPLETE = 1;        // got a full command
const int PARSE_PROTOCOL_ERROR = 2;  // buffer doesn't look like valid RESP

struct ParseResult {
    int status;
    vector<string> args;   // filled only if status == PARSE_COMPLETE
    size_t bytesConsumed;  // how much to erase from the front of the buffer, if COMPLETE
};

// TODO: implement this.
// Given the raw bytes currently sitting in `buf`, try to extract ONE
// complete RESP command (an array of bulk strings).
//
// Steps to implement (from the walkthrough):
// 1. Check buf[0] == '*'. If buf is too short to even see a full first
//    line (no '\r\n' found yet), return PARSE_INCOMPLETE.
//    If buf[0] isn't '*', return PARSE_PROTOCOL_ERROR.
// 2. Parse the number after '*' up to '\r\n' -> argc.
// 3. For i in [0, argc):
//    a. Need a line starting with '$' -> if not found yet (no '\r\n'
//       in what remains), return PARSE_INCOMPLETE. If it doesn't start
//       with '$', return PARSE_PROTOCOL_ERROR.
//    b. Parse the number after '$' up to '\r\n' -> len.
//    c. Check there are at least `len + 2` bytes remaining after that
//       line (the +2 is the trailing \r\n). If not, return PARSE_INCOMPLETE.
//    d. Extract those `len` bytes as one argument, confirm the next
//       2 bytes are exactly "\r\n" (else PARSE_PROTOCOL_ERROR), advance
//       your cursor past all of it.
// 4. Once all argc elements are parsed, return PARSE_COMPLETE with the
//    args vector and bytesConsumed = however far your cursor got.
ParseResult tryParseCommand(const string& buf) {
    ParseResult result;
    result.status = PARSE_INCOMPLETE;
    result.bytesConsumed = 0;

    if (buf.size() < 1) {
        return result;  // nothing arrived yet
    }
    if (buf[0] != '*') {
        result.status = PARSE_PROTOCOL_ERROR;
        return result;
    }

    // ---- step 1/2: parse "*<argc>\r\n" ----
    size_t lineEnd = buf.find("\r\n", 0);
    if (lineEnd == string::npos) {
        return result;  // haven't seen the end of the first line yet
    }
    string countStr = buf.substr(1, lineEnd - 1);  // between '*' and "\r\n"
    int argc = stoi(countStr);

    size_t pos = lineEnd + 2;  // cursor: move past "*3\r\n"
    vector<string> args;

    // ---- step 3: parse each "$<len>\r\n<data>\r\n" element ----
    for (int i = 0; i < argc; i++) {
        if (pos >= buf.size()) {
            return result;  // ran out of buffer before seeing '$'
        }
        if (buf[pos] != '$') {
            result.status = PARSE_PROTOCOL_ERROR;
            return result;
        }

        size_t lenLineEnd = buf.find("\r\n", pos);
        if (lenLineEnd == string::npos) {
            return result;  // length line not finished yet
        }
        string lenStr = buf.substr(pos + 1, lenLineEnd - (pos + 1));
        int len = stoi(lenStr);

        pos = lenLineEnd + 2;  // cursor: move past "$3\r\n"

        // need `len` data bytes plus a trailing "\r\n"
        if (buf.size() < pos + len + 2) {
            return result;  // data not fully arrived yet
        }

        string value = buf.substr(pos, len);
        if (buf[pos + len] != '\r' || buf[pos + len + 1] != '\n') {
            result.status = PARSE_PROTOCOL_ERROR;
            return result;
        }

        args.push_back(value);
        pos = pos + len + 2;  // move past the value and its trailing "\r\n"
    }

    // ---- step 4: all argc elements parsed successfully ----
    result.status = PARSE_COMPLETE;
    result.args = args;
    result.bytesConsumed = pos;
    return result;
}

// ---- test harness, no networking ----
void runTest(const string& label, const string& input) {
    ParseResult r = tryParseCommand(input);
    printf("[%s] status=%d bytesConsumed=%zu args=[",
           label.c_str(), r.status, r.bytesConsumed);
    for (size_t i = 0; i < r.args.size(); i++) {
        printf("\"%s\" ", r.args[i].c_str());
    }
    printf("]\n");
}

int main() {
    // A complete SET command.
    runTest("complete", "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$7\r\nnagaraj\r\n");

    // Same command, but cut off mid bulk-string ("nagaraj" truncated to "na").
    runTest("truncated-value", "*3\r\n$3\r\nSET\r\n$4\r\nname\r\n$7\r\nna");

    // Cut off before even finishing the array header line.
    runTest("truncated-header", "*3\r\n$3\r\nSE");

    // A simple PING (no args).
    runTest("ping", "*1\r\n$4\r\nPING\r\n");

    return 0;
}
