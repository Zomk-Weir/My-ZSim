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

// modified by wei wu on 251211: Added PC access recording functionality

#ifndef PC_ACCESS_RECORDER_H_
#define PC_ACCESS_RECORDER_H_

#include <stdint.h>
#include <unordered_map>
#include <vector>
#include "galloc.h"
#include "locks.h"
#include "memory_hierarchy.h"

// Structure to hold access statistics for a specific PC
struct PCAccessStats {
    uint64_t totalAccesses;    // Total number of accesses (hits + misses)
    uint64_t readAccesses;     // Number of read accesses (GETS)
    uint64_t writeAccesses;    // Number of write accesses (GETX)
    
    PCAccessStats() : totalAccesses(0), readAccesses(0), writeAccesses(0) {}
    
    void recordAccess(bool isRead) {
        totalAccesses++;
        if (isRead) {
            readAccesses++;
        } else {
            writeAccesses++;
        }
    }
};

// Main PC access recorder class
class PCAccessRecorder : public GlobAlloc {
private:
    std::unordered_map<Address, PCAccessStats> pcToStats;  // PC -> access stats mapping
    mutable lock_t recorderLock;  // mutable to allow locking in const methods
    uint64_t maxRecords;        // Maximum number of unique PCs to record
    uint64_t totalAccesses;     // Total number of accesses recorded
    bool enabled;               // Whether recording is enabled
    const char* cacheName;      // Name of the cache this recorder belongs to
    
public:
    PCAccessRecorder(const char* name, uint64_t maxRecs, bool enable);
    ~PCAccessRecorder();
    
    // Record a PC access
    void recordAccess(Address pc, bool isRead);
    
    // Get statistics
    uint64_t getTotalAccesses() const { return totalAccesses; }
    uint64_t getUniquePCs() const { return pcToStats.size(); }
    bool isEnabled() const { return enabled; }
    
    // Export data for HDF5 output
    void exportToHDF5(const char* filename) const;
    
    // Clear all records
    void clear();
    
    // Get all PC addresses (for iteration)
    std::vector<Address> getAllPCs() const;
    
    // Get stats for a specific PC
    const PCAccessStats* getStatsForPC(Address pc) const;
};

// Global PC access recorder manager
class PCAccessRecorderManager : public GlobAlloc {
private:
    std::unordered_map<std::string, PCAccessRecorder*> recorders;
    mutable lock_t managerLock;  // mutable to allow locking in const methods
    bool globalEnabled;
    uint64_t globalMaxRecords;
    
public:
    PCAccessRecorderManager();
    ~PCAccessRecorderManager();
    
    // Initialize from configuration
    void init(bool enabled, uint64_t maxRecords);
    
    // Get or create recorder for a cache
    PCAccessRecorder* getRecorder(const char* cacheName);
    
    // Export all recorders to HDF5 files
    void exportAllToHDF5(const char* outputDir) const;
    
    // Get global statistics
    uint64_t getTotalAccessesAcrossAllCaches() const;
    
    bool isEnabled() const { return globalEnabled; }
};

// Global instance
extern PCAccessRecorderManager* g_pcAccessRecorderManager;

#endif  // PC_ACCESS_RECORDER_H_
