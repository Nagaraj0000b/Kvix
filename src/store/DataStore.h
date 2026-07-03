#pragma once
#include <string>
#include <vector>
#include <list>
#include <unordered_map>
using namespace std;

// Everything we keep about one stored key.
// - expiryTimestamp: 0 means "never expires", otherwise a unix time (seconds)
//   after which the key counts as dead.
// - recencyNode: where this key currently sits in the recency list, so we can
//   move it to the front or remove it in O(1) without searching the list.
struct StoredItem {
    string value;
    long long expiryTimestamp;
    list<string>::iterator recencyNode;
};

// The storage engine: a hash map of keys plus TTL expiry and LRU eviction.
// This is the whole "store" layer - the network and command layers only ever
// talk to it through these methods, never touch the map directly.
class DataStore {
public:
    DataStore();

    // does this key exist right now (and is it not expired)?
    bool hasKey(const string& key);

    // store value under key. clears any old TTL. evicts the least-recently-used
    // key first if the store is already full.
    void setValue(const string& key, const string& value);

    // if key exists and is not expired, copy its value into outValue and
    // return true; otherwise return false.
    bool getValue(const string& key, string& outValue);

    // remove key. returns true if it existed.
    bool deleteKey(const string& key);

    // give key a time-to-live of `seconds`. returns true if the key existed.
    bool setExpiry(const string& key, long long seconds);

    // -2 if the key is missing, -1 if it exists with no TTL, else seconds left.
    long long getTimeToLive(const string& key);

    // add 1 to the key's integer value (missing key counts as 0). returns true
    // on success, false if the existing value is not a valid integer. the new
    // number is written to outNewValue.
    bool incrementValue(const string& key, long long& outNewValue);

    // every key that is currently alive (not expired).
    vector<string> getAllKeys();

    // wipe everything.
    void clearAll();

    // active expiration: delete every key that is currently expired.
    void removeExpiredKeys();

    // ---- used by the snapshot save/load ----

    // put a key back exactly as it was, KEEPING its expiry (unlike setValue,
    // which would clear the TTL). used when loading a snapshot from disk.
    void restoreItem(const string& key, const string& value, long long expiryTimestamp);

    // read-only view of every stored item, for dumping to disk.
    const unordered_map<string, StoredItem>& getAllItems() const;

private:
    unordered_map<string, StoredItem> items;
    list<string> recencyOrder; // front = most recently used, back = least
    size_t maxKeyCount;

    bool isItemExpired(const StoredItem& item);
    void markAsRecentlyUsed(const string& key);
    void evictLeastRecentlyUsedIfFull();
};
