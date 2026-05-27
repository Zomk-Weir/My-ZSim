/** $lic$
 * Copyright (C) 2012-2015 by Massachusetts Institute of Technology
 * Copyright (C) 2010-2013 by The Board of Trustees of Stanford University
 *
 * This file is part of zsim.
 *
 * zsim is free software; you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software
 * Foundation, version 2.
 *
 * If you use this software in your research, we request that you reference
 * the zsim paper ("ZSim: Fast and Accurate Microarchitectural Simulation of
 * Thousand-Core Systems", Sanchez and Kozyrakis, ISCA-40, June 2013) as the
 * source of the simulator in any publications that use this software, and that
 * you send us a citation of your work.
 *
 * zsim is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

// modified by wei wu on 250816: Added cache miss recording functionality

#ifndef CACHE_MISS_RECORDER_H_
#define CACHE_MISS_RECORDER_H_

#include <stdint.h>
#include <unordered_map>
#include <vector>
#include "galloc.h"
#include "locks.h"
#include "memory_hierarchy.h"

// Structure to hold a single cache miss record
struct CacheMissRecord {
    uint64_t cycle;
    Address address;
    
    CacheMissRecord() : cycle(0), address(0) {}
    CacheMissRecord(uint64_t c, Address a) : cycle(c), address(a) {}
};

// Enum for miss types
enum MissType {
    MISS_GETS = 0,    // mGETS: Read miss (I->S/E)
    MISS_GETXIM = 1,  // mGETXIM: Write miss (I->M)  
    MISS_GETXSM = 2,  // mGETXSM: Upgrade miss (S->M)
    MISS_TYPE_COUNT = 3
};

// Convert miss type to string
inline const char* MissTypeName(MissType type) {
    switch(type) {
        case MISS_GETS: return "mGETS";
        case MISS_GETXIM: return "mGETXIM"; 
        case MISS_GETXSM: return "mGETXSM";
        default: return "unknown";
    }
}

// Structure to hold miss records for a specific PC and miss type
struct PCMissTypeRecords {
    std::vector<CacheMissRecord> records[MISS_TYPE_COUNT];
    
    void addRecord(MissType type, uint64_t cycle, Address address) {
        if (type < MISS_TYPE_COUNT) {
            records[type].emplace_back(cycle, address);
        }
    }
    
    size_t getTotalSize() const {
        size_t total = 0;
        for (int i = 0; i < MISS_TYPE_COUNT; i++) {
            total += records[i].size();
        }
        return total;
    }
    
    size_t getTypeSize(MissType type) const {
        return (type < MISS_TYPE_COUNT) ? records[type].size() : 0;
    }
};

// Main cache miss recorder class
class CacheMissRecorder : public GlobAlloc {
private:
    std::unordered_map<Address, PCMissTypeRecords> pcToRecords;  // PC -> miss records mapping
    mutable lock_t recorderLock;  // mutable to allow locking in const methods
    uint64_t maxRecords;        // Maximum number of records to store
    uint64_t totalRecords;      // Current total number of records
    bool enabled;               // Whether recording is enabled
    const char* cacheName;      // Name of the cache this recorder belongs to
    
public:
    CacheMissRecorder(const char* name, uint64_t maxRecs, bool enable);
    ~CacheMissRecorder();
    
    // Record a cache miss with type
    void recordMiss(Address pc, MissType type, uint64_t cycle, Address address);
    
    // Get statistics
    uint64_t getTotalRecords() const { return totalRecords; }
    uint64_t getUniquePCs() const { return pcToRecords.size(); }
    bool isEnabled() const { return enabled; }
    
    // Export data for HDF5 output
    void exportToHDF5(const char* filename) const;
    
    // Clear all records
    void clear();
    
    // Get all PC addresses (for iteration)
    std::vector<Address> getAllPCs() const;
    
    // Get records for a specific PC
    const PCMissTypeRecords* getRecordsForPC(Address pc) const;
};

// Global cache miss recorder manager
class CacheMissRecorderManager : public GlobAlloc {
private:
    std::unordered_map<std::string, CacheMissRecorder*> recorders;
    mutable lock_t managerLock;  // mutable to allow locking in const methods
    bool globalEnabled;
    uint64_t globalMaxRecords;
    
public:
    CacheMissRecorderManager();
    ~CacheMissRecorderManager();
    
    // Initialize from configuration
    void init(bool enabled, uint64_t maxRecords);
    
    // Get or create recorder for a cache
    CacheMissRecorder* getRecorder(const char* cacheName);
    
    // Export all recorders to HDF5 files
    void exportAllToHDF5(const char* outputDir) const;
    
    // Get global statistics
    uint64_t getTotalRecordsAcrossAllCaches() const;
    
    bool isEnabled() const { return globalEnabled; }
};

// Global instance
extern CacheMissRecorderManager* g_cacheMissRecorderManager;

#endif  // CACHE_MISS_RECORDER_H_ 