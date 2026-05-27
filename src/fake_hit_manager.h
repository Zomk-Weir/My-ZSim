#ifndef FAKE_HIT_MANAGER_H_
#define FAKE_HIT_MANAGER_H_

#include <unordered_set>
#include <unordered_map>
#include <string>
#include "memory_hierarchy.h"
#include "cache_miss_recorder.h"

// Fake hit statistics for a specific PC
struct FakeHitStats {
    uint64_t getsCount;
    uint64_t getximCount;
    uint64_t getxsmCount;
    
    FakeHitStats() : getsCount(0), getximCount(0), getxsmCount(0) {}
};

// Fake hit manager class
class FakeHitManager {
private:
    std::unordered_set<Address> fakeHitPCs;  // PCs to fake hit
    std::unordered_map<std::string, std::unordered_map<Address, FakeHitStats>> cacheStats;  // cache -> PC -> stats
    
public:
    FakeHitManager();
    
    // Initialize with predefined PCs (you can modify these addresses)
    void initializeFakeHitPCs();
    
    // Check if a PC should be fake hit
    bool shouldFakeHit(Address pc) const;
    
    // Record a fake hit
    void recordFakeHit(const std::string& cacheName, Address pc, MissType type);
    
    // Print all statistics at the end of simulation
    void printStatistics() const;
    
    // Check if cache is L2 or L3
    bool isL1Cache(const std::string& cacheName) const;
    bool isL2Cache(const std::string& cacheName) const;
    bool isL3Cache(const std::string& cacheName) const;
};

// Global instance
extern FakeHitManager* g_fakeHitManager;

#endif  // FAKE_HIT_MANAGER_H_