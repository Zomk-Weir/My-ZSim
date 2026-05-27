#include "fake_hit_manager.h"
#include "cache_miss_recorder.h"
#include "log.h"
#include <iostream>
#include <iomanip>

// Global instance
FakeHitManager* g_fakeHitManager = nullptr;

FakeHitManager::FakeHitManager() {
    initializeFakeHitPCs();
}

void FakeHitManager::initializeFakeHitPCs() {
    // TODO: Replace these with your target PC addresses
    // Example PC addresses - modify these to your actual target PCs

    //carribot's missed PCs
    fakeHitPCs.insert(0x408a20);//top 1
    fakeHitPCs.insert(0x408a29);//top 2 
    fakeHitPCs.insert(0x408a33);//top 3
    fakeHitPCs.insert(0x408f4f);
    fakeHitPCs.insert(0x408f55);
    fakeHitPCs.insert(0x408e05);
    fakeHitPCs.insert(0x408f49); 
    fakeHitPCs.insert(0x408ba4);
    fakeHitPCs.insert(0x408d05);
    fakeHitPCs.insert(0x408d3d);
    fakeHitPCs.insert(0x408bd2);
    
    info("FakeHitManager: Initialized with %zu target PCs for fake hits", fakeHitPCs.size());
}

bool FakeHitManager::shouldFakeHit(Address pc) const {
    return fakeHitPCs.find(pc) != fakeHitPCs.end();
}

void FakeHitManager::recordFakeHit(const std::string& cacheName, Address pc, MissType type) {
    auto& pcStats = cacheStats[cacheName][pc];
    
    switch(type) {
        case MISS_GETS:
            pcStats.getsCount++;
            break;
        case MISS_GETXIM:
            pcStats.getximCount++;
            break;
        case MISS_GETXSM:
            pcStats.getxsmCount++;
            break;
        default:
            break;
    }
}

bool FakeHitManager::isL1Cache(const std::string& cacheName) const {
    return cacheName.find("l1") != std::string::npos || 
           cacheName.find("L1") != std::string::npos;
}

bool FakeHitManager::isL2Cache(const std::string& cacheName) const {
    return cacheName.find("l2") != std::string::npos || 
           cacheName.find("L2") != std::string::npos;
}

bool FakeHitManager::isL3Cache(const std::string& cacheName) const {
    return cacheName.find("l3") != std::string::npos || 
           cacheName.find("L3") != std::string::npos;
}

void FakeHitManager::printStatistics() const {
    std::cout << "\n" << std::string(80, '=') << std::endl;
    std::cout << "FAKE HIT STATISTICS SUMMARY" << std::endl;
    std::cout << std::string(80, '=') << std::endl;
    
    if (cacheStats.empty()) {
        std::cout << "No fake hits recorded." << std::endl;
        std::cout << std::string(80, '=') << std::endl;
        return;
    }

    // first print L1 cache fake hits
    std::cout << "\nL1 CACHE FAKE HITS:" << std::endl;
    std::cout << std::string(50, '-') << std::endl;
    
    for (const auto& cacheEntry : cacheStats) {
        const std::string& cacheName = cacheEntry.first;
        const auto& pcStatsMap = cacheEntry.second;
        
        if (isL1Cache(cacheName)) {
            std::cout << "Cache: " << cacheName << std::endl;
            std::cout << std::setw(12) << "PC" << std::setw(10) << "GETS" 
                      << std::setw(10) << "GETXIM" << std::setw(10) << "GETXSM" 
                      << std::setw(10) << "Total" << std::endl;
            
            uint64_t totalGets = 0, totalGetxim = 0, totalGetxsm = 0;
            
            for (const auto& pcEntry : pcStatsMap) {
                Address pc = pcEntry.first;
                const FakeHitStats& stats = pcEntry.second;
                uint64_t total = stats.getsCount + stats.getximCount + stats.getxsmCount;
                
                std::cout << "0x" << std::hex << std::setw(8) << pc << std::dec
                          << std::setw(10) << stats.getsCount
                          << std::setw(10) << stats.getximCount  
                          << std::setw(10) << stats.getxsmCount
                          << std::setw(10) << total << std::endl;
                
                totalGets += stats.getsCount;
                totalGetxim += stats.getximCount;
                totalGetxsm += stats.getxsmCount;
            }
            
            std::cout << std::string(50, '-') << std::endl;
            std::cout << std::setw(12) << "Total:" 
                      << std::setw(10) << totalGets
                      << std::setw(10) << totalGetxim
                      << std::setw(10) << totalGetxsm
                      << std::setw(10) << (totalGets + totalGetxim + totalGetxsm) << std::endl;
            std::cout << std::endl;
        }
    }
    
    // Separate L2 and L3 statistics
    std::cout << "\nL2 CACHE FAKE HITS:" << std::endl;
    std::cout << std::string(50, '-') << std::endl;
    
    for (const auto& cacheEntry : cacheStats) {
        const std::string& cacheName = cacheEntry.first;
        const auto& pcStatsMap = cacheEntry.second;
        
        if (isL2Cache(cacheName)) {
            std::cout << "Cache: " << cacheName << std::endl;
            std::cout << std::setw(12) << "PC" << std::setw(10) << "GETS" 
                      << std::setw(10) << "GETXIM" << std::setw(10) << "GETXSM" 
                      << std::setw(10) << "Total" << std::endl;
            
            uint64_t totalGets = 0, totalGetxim = 0, totalGetxsm = 0;
            
            for (const auto& pcEntry : pcStatsMap) {
                Address pc = pcEntry.first;
                const FakeHitStats& stats = pcEntry.second;
                uint64_t total = stats.getsCount + stats.getximCount + stats.getxsmCount;
                
                std::cout << "0x" << std::hex << std::setw(8) << pc << std::dec
                          << std::setw(10) << stats.getsCount
                          << std::setw(10) << stats.getximCount  
                          << std::setw(10) << stats.getxsmCount
                          << std::setw(10) << total << std::endl;
                
                totalGets += stats.getsCount;
                totalGetxim += stats.getximCount;
                totalGetxsm += stats.getxsmCount;
            }
            
            std::cout << std::string(50, '-') << std::endl;
            std::cout << std::setw(12) << "Total:" 
                      << std::setw(10) << totalGets
                      << std::setw(10) << totalGetxim
                      << std::setw(10) << totalGetxsm
                      << std::setw(10) << (totalGets + totalGetxim + totalGetxsm) << std::endl;
            std::cout << std::endl;
        }
    }
    

    // modified by wei wu on 251212: print L3 cache fake hits
    std::cout << "\nL3 CACHE FAKE HITS:" << std::endl;
    std::cout << std::string(50, '-') << std::endl;
    
    for (const auto& cacheEntry : cacheStats) {
        const std::string& cacheName = cacheEntry.first;
        const auto& pcStatsMap = cacheEntry.second;
        
        if (isL3Cache(cacheName)) {
            std::cout << "Cache: " << cacheName << std::endl;
            std::cout << std::setw(12) << "PC" << std::setw(10) << "GETS" 
                      << std::setw(10) << "GETXIM" << std::setw(10) << "GETXSM" 
                      << std::setw(10) << "Total" << std::endl;
            
            uint64_t totalGets = 0, totalGetxim = 0, totalGetxsm = 0;
            
            for (const auto& pcEntry : pcStatsMap) {
                Address pc = pcEntry.first;
                const FakeHitStats& stats = pcEntry.second;
                uint64_t total = stats.getsCount + stats.getximCount + stats.getxsmCount;
                
                std::cout << "0x" << std::hex << std::setw(8) << pc << std::dec
                          << std::setw(10) << stats.getsCount
                          << std::setw(10) << stats.getximCount  
                          << std::setw(10) << stats.getxsmCount
                          << std::setw(10) << total << std::endl;
                
                totalGets += stats.getsCount;
                totalGetxim += stats.getximCount;
                totalGetxsm += stats.getxsmCount;
            }
            
            std::cout << std::string(50, '-') << std::endl;
            std::cout << std::setw(12) << "Total:" 
                      << std::setw(10) << totalGets
                      << std::setw(10) << totalGetxim
                      << std::setw(10) << totalGetxsm
                      << std::setw(10) << (totalGets + totalGetxim + totalGetxsm) << std::endl;
            std::cout << std::endl;
        }
    }
    
    std::cout << std::string(80, '=') << std::endl;
}