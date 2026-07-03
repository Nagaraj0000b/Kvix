#include "DataStore.h"
#include <ctime> // time()

DataStore::DataStore() {
    // Small on purpose so LRU eviction is easy to watch while testing by hand.
    // Counts number of keys, not real memory - a documented simplification.
    maxKeyCount = 5;
}

bool DataStore::isItemExpired(const StoredItem& item) {
    return item.expiryTimestamp != 0 && time(nullptr) >= item.expiryTimestamp;
}

// Move this key's node to the front of the recency list (most recently used).
void DataStore::markAsRecentlyUsed(const string& key) {
    recencyOrder.erase(items[key].recencyNode);
    recencyOrder.push_front(key);
    items[key].recencyNode = recencyOrder.begin();
}

// If we are already at the cap, drop the least-recently-used key (back of the
// recency list) to make room for a new one.
void DataStore::evictLeastRecentlyUsedIfFull() {
    if (items.size() < maxKeyCount) {
        return;
    }
    string leastUsedKey = recencyOrder.back();
    items.erase(leastUsedKey);
    recencyOrder.pop_back();
}

bool DataStore::hasKey(const string& key) {
    unordered_map<string, StoredItem>::iterator found = items.find(key);
    if (found == items.end()) {
        return false;
    }
    if (isItemExpired(found->second)) {
        // lazy expiration: found it dead, so clean it up now
        recencyOrder.erase(found->second.recencyNode);
        items.erase(found);
        return false;
    }
    return true;
}

void DataStore::setValue(const string& key, const string& value) {
    unordered_map<string, StoredItem>::iterator found = items.find(key);
    if (found == items.end()) {
        // brand-new key: make room if needed, then insert at the front
        evictLeastRecentlyUsedIfFull();
        StoredItem item;
        item.value = value;
        item.expiryTimestamp = 0;
        recencyOrder.push_front(key);
        item.recencyNode = recencyOrder.begin();
        items[key] = item;
    } else {
        // existing key: overwrite value, clear any old TTL, mark as used
        found->second.value = value;
        found->second.expiryTimestamp = 0;
        markAsRecentlyUsed(key);
    }
}

bool DataStore::getValue(const string& key, string& outValue) {
    unordered_map<string, StoredItem>::iterator found = items.find(key);
    if (found == items.end()) {
        return false;
    }
    if (isItemExpired(found->second)) {
        recencyOrder.erase(found->second.recencyNode);
        items.erase(found);
        return false;
    }
    markAsRecentlyUsed(key); // a read counts as "used" too
    outValue = items[key].value;
    return true;
}

bool DataStore::deleteKey(const string& key) {
    unordered_map<string, StoredItem>::iterator found = items.find(key);
    if (found == items.end()) {
        return false;
    }
    recencyOrder.erase(found->second.recencyNode);
    items.erase(found);
    return true;
}

bool DataStore::setExpiry(const string& key, long long seconds) {
    if (!hasKey(key)) {
        return false;
    }
    items[key].expiryTimestamp = time(nullptr) + seconds;
    return true;
}

long long DataStore::getTimeToLive(const string& key) {
    unordered_map<string, StoredItem>::iterator found = items.find(key);
    if (found == items.end() || isItemExpired(found->second)) {
        return -2; // key does not exist
    }
    if (found->second.expiryTimestamp == 0) {
        return -1; // exists, but no TTL set
    }
    return found->second.expiryTimestamp - time(nullptr);
}

bool DataStore::incrementValue(const string& key, long long& outNewValue) {
    long long current = 0;
    bool keyExists = hasKey(key);

    if (keyExists) {
        try {
            current = stoll(items[key].value);
        } catch (...) {
            return false; // existing value isn't a number
        }
    }

    long long updated = current + 1;
    if (keyExists) {
        // in-place update: do NOT touch expiryTimestamp, so the TTL survives
        items[key].value = to_string(updated);
        markAsRecentlyUsed(key);
    } else {
        evictLeastRecentlyUsedIfFull();
        StoredItem item;
        item.value = to_string(updated);
        item.expiryTimestamp = 0;
        recencyOrder.push_front(key);
        item.recencyNode = recencyOrder.begin();
        items[key] = item;
    }
    outNewValue = updated;
    return true;
}

vector<string> DataStore::getAllKeys() {
    vector<string> result;
    for (unordered_map<string, StoredItem>::iterator it = items.begin(); it != items.end(); ++it) {
        if (!isItemExpired(it->second)) {
            result.push_back(it->first);
        }
    }
    return result;
}

void DataStore::clearAll() {
    items.clear();
    recencyOrder.clear(); // otherwise the recency list keeps dangling keys
}

void DataStore::removeExpiredKeys() {
    unordered_map<string, StoredItem>::iterator it = items.begin();
    while (it != items.end()) {
        if (isItemExpired(it->second)) {
            recencyOrder.erase(it->second.recencyNode);
            it = items.erase(it);
        } else {
            ++it;
        }
    }
}

void DataStore::restoreItem(const string& key, const string& value, long long expiryTimestamp) {
    StoredItem item;
    item.value = value;
    item.expiryTimestamp = expiryTimestamp; // keep the saved TTL as-is
    recencyOrder.push_front(key);
    item.recencyNode = recencyOrder.begin();
    items[key] = item;
}

const unordered_map<string, StoredItem>& DataStore::getAllItems() const {
    return items;
}
