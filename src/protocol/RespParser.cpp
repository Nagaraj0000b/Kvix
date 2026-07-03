#include "RespParser.h"

ParseResult tryParseCommand(const string& buffer) {
    ParseResult result;
    result.status = PARSE_INCOMPLETE;
    result.bytesConsumed = 0;

    if (buffer.size() < 1) {
        return result; // nothing arrived yet
    }
    if (buffer[0] != '*') {
        result.status = PARSE_PROTOCOL_ERROR;
        return result;
    }

    // ---- parse the "*<count>\r\n" header line ----
    size_t headerEnd = buffer.find("\r\n", 0);
    if (headerEnd == string::npos) {
        return result; // header line not finished yet
    }
    string countText = buffer.substr(1, headerEnd - 1); // between '*' and "\r\n"
    int elementCount = stoi(countText);

    size_t cursor = headerEnd + 2; // move past "*3\r\n"
    vector<string> parsedArgs;

    // ---- parse each "$<length>\r\n<data>\r\n" element ----
    for (int i = 0; i < elementCount; i++) {
        if (cursor >= buffer.size()) {
            return result; // ran out of bytes before the next element
        }
        if (buffer[cursor] != '$') {
            result.status = PARSE_PROTOCOL_ERROR;
            return result;
        }

        size_t lengthLineEnd = buffer.find("\r\n", cursor);
        if (lengthLineEnd == string::npos) {
            return result; // length line not finished yet
        }
        string lengthText = buffer.substr(cursor + 1, lengthLineEnd - (cursor + 1));
        int dataLength = stoi(lengthText);

        cursor = lengthLineEnd + 2; // move past "$3\r\n"

        // need `dataLength` bytes of data plus a trailing "\r\n"
        if (buffer.size() < cursor + dataLength + 2) {
            return result; // data not fully arrived yet
        }

        string data = buffer.substr(cursor, dataLength);
        if (buffer[cursor + dataLength] != '\r' || buffer[cursor + dataLength + 1] != '\n') {
            result.status = PARSE_PROTOCOL_ERROR;
            return result;
        }

        parsedArgs.push_back(data);
        cursor = cursor + dataLength + 2; // move past the data and its "\r\n"
    }

    result.status = PARSE_COMPLETE;
    result.args = parsedArgs;
    result.bytesConsumed = cursor;
    return result;
}

string encodeCommand(const vector<string>& args) {
    string encoded = "*" + to_string(args.size()) + "\r\n";
    for (size_t i = 0; i < args.size(); i++) {
        encoded += "$" + to_string(args[i].size()) + "\r\n" + args[i] + "\r\n";
    }
    return encoded;
}
