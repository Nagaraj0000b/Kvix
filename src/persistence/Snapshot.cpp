#include "Snapshot.h"
#include "../protocol/RespParser.h"
#include <fstream>
#include <iterator> // istreambuf_iterator

Snapshot::Snapshot(const string& path) {
    filePath = path;
}

void Snapshot::save(DataStore& store) {
    store.removeExpiredKeys(); // don't bother saving keys that are already dead

    ofstream out(filePath, ios::binary | ios::trunc);

    // Each key becomes one record of three fields: [key, value, expiry].
    // We reuse the RESP encoder so the file format matches the AOF's.
    const unordered_map<string, StoredItem>& items = store.getAllItems();
    for (unordered_map<string, StoredItem>::const_iterator it = items.begin(); it != items.end(); ++it) {
        vector<string> record;
        record.push_back(it->first);                         // key
        record.push_back(it->second.value);                  // value
        record.push_back(to_string(it->second.expiryTimestamp)); // expiry
        out << encodeCommand(record);
    }
    out.close();
}

void Snapshot::load(DataStore& store) {
    ifstream in(filePath, ios::binary);
    if (!in.is_open()) {
        return; // no snapshot yet
    }
    string contents((istreambuf_iterator<char>(in)), istreambuf_iterator<char>());
    in.close();

    size_t offset = 0;
    while (offset < contents.size()) {
        string remaining = contents.substr(offset);
        ParseResult parsed = tryParseCommand(remaining);
        if (parsed.status != PARSE_COMPLETE || parsed.args.size() != 3) {
            break;
        }
        string key = parsed.args[0];
        string value = parsed.args[1];
        long long expiry = stoll(parsed.args[2]);
        store.restoreItem(key, value, expiry); // keeps the saved TTL
        offset += parsed.bytesConsumed;
    }
}
