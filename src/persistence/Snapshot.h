#pragma once
#include <string>
#include "../store/DataStore.h"
using namespace std;

// Snapshot: a full dump of the whole store to disk, all at once (a
// "checkpoint"). Faster to restore than replaying a long AOF, but on its own
// it loses anything written after the dump. We use it together with the AOF:
// snapshot for the base state, AOF for whatever happened since.
class Snapshot {
public:
    Snapshot(const string& filePath);

    // write the current contents of the store to disk
    void save(DataStore& store);

    // load a previously-saved snapshot back into the store (does nothing if
    // there is no snapshot file yet)
    void load(DataStore& store);

private:
    string filePath;
};
