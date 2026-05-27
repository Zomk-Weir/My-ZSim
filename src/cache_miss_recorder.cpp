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

#include "cache_miss_recorder.h"
#include <hdf5.h>
#include <hdf5_hl.h>
#include <string>
#include <cstring>
#include "log.h"
#include "zsim.h"

// Global instance
CacheMissRecorderManager* g_cacheMissRecorderManager = nullptr;

// CacheMissRecorder implementation
CacheMissRecorder::CacheMissRecorder(const char* name, uint64_t maxRecs, bool enable) 
    : maxRecords(maxRecs), totalRecords(0), enabled(enable) {
    cacheName = gm_strdup(name);
    futex_init(&recorderLock);
}

CacheMissRecorder::~CacheMissRecorder() {
    if (cacheName) {
        gm_free((void*)cacheName);
    }
}

void CacheMissRecorder::recordMiss(Address pc, MissType type, uint64_t cycle, Address address) {
    if (!enabled) {
        info("[%s] Recording disabled, skipping miss record", cacheName);
        return;
    }
    
    if (totalRecords >= maxRecords) {
        if (totalRecords == maxRecords) {
            info("[%s] Reached max records limit (%lu), stopping recording", cacheName, maxRecords);
        }
        return;
    }
    
    futex_lock(&recorderLock);
    
    // Check again after acquiring lock
    if (totalRecords >= maxRecords) {
        futex_unlock(&recorderLock);
        return;
    }
    
    // Add record to the PC's record list with type
    pcToRecords[pc].addRecord(type, cycle, address);
    totalRecords++;
    
    // Debug info for first few records
    // if (totalRecords <= 10) {
    //     info("[%s] Recorded miss #%lu: PC=0x%lx, type=%s, cycle=%lu, addr=0x%lx", 
    //          cacheName, totalRecords, pc, MissTypeName(type), cycle, address);
    // }
    
    futex_unlock(&recorderLock);
}

void CacheMissRecorder::clear() {
    futex_lock(&recorderLock);
    pcToRecords.clear();
    totalRecords = 0;
    futex_unlock(&recorderLock);
}

std::vector<Address> CacheMissRecorder::getAllPCs() const {
    std::vector<Address> pcs;
    futex_lock(&recorderLock);
    pcs.reserve(pcToRecords.size());
    for (const auto& pair : pcToRecords) {
        pcs.push_back(pair.first);
    }
    futex_unlock(&recorderLock);
    return pcs;
}

const PCMissTypeRecords* CacheMissRecorder::getRecordsForPC(Address pc) const {
    futex_lock(&recorderLock);
    auto it = pcToRecords.find(pc);
    const PCMissTypeRecords* result = (it != pcToRecords.end()) ? &it->second : nullptr;
    futex_unlock(&recorderLock);
    return result;
}

void CacheMissRecorder::exportToHDF5(const char* filename) const {
    if (totalRecords == 0) {
        info("[%s] No cache miss records to export", cacheName);
        return;
    }
    
    futex_lock(&recorderLock);
    
    // Create HDF5 file
    hid_t file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_id < 0) {
        warn("[%s] Failed to create HDF5 file: %s", cacheName, filename);
        futex_unlock(&recorderLock);
        return;
    }
    
    // Create group for this cache
    hid_t cache_group = H5Gcreate2(file_id, cacheName, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (cache_group < 0) {
        warn("[%s] Failed to create cache group in HDF5 file", cacheName);
        H5Fclose(file_id);
        futex_unlock(&recorderLock);
        return;
    }
    
    // Write metadata
    hsize_t scalar_dims[1] = {1};
    hid_t scalar_space = H5Screate_simple(1, scalar_dims, NULL);
    hid_t uint64_type = H5Tcopy(H5T_NATIVE_UINT64);
    
    // Total records
    hid_t total_records_dataset = H5Dcreate2(cache_group, "total_records", uint64_type, 
                                           scalar_space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Dwrite(total_records_dataset, uint64_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &totalRecords);
    H5Dclose(total_records_dataset);
    
    // Unique PCs
    uint64_t uniquePCs = pcToRecords.size();
    hid_t unique_pcs_dataset = H5Dcreate2(cache_group, "unique_pcs", uint64_type, 
                                        scalar_space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Dwrite(unique_pcs_dataset, uint64_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &uniquePCs);
    H5Dclose(unique_pcs_dataset);
    
    H5Sclose(scalar_space);
    H5Tclose(uint64_type);
    
    // Write PC-indexed data with miss type classification
    for (const auto& pcEntry : pcToRecords) {
        Address pc = pcEntry.first;
        const PCMissTypeRecords& records = pcEntry.second;
        
        // Create group for this PC
        char pc_group_name[32];
        snprintf(pc_group_name, sizeof(pc_group_name), "PC_0x%lx", pc);
        hid_t pc_group = H5Gcreate2(cache_group, pc_group_name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
        
        if (pc_group < 0) {
            warn("[%s] Failed to create PC group: %s", cacheName, pc_group_name);
            continue;
        }
        
        // Write data for each miss type
        for (int missTypeIdx = 0; missTypeIdx < MISS_TYPE_COUNT; missTypeIdx++) {
            MissType missType = static_cast<MissType>(missTypeIdx);
            const std::vector<CacheMissRecord>& typeRecords = records.records[missTypeIdx];
            
            if (typeRecords.empty()) {
                continue; // Skip empty miss types
            }
            
            // Create group for this miss type
            const char* missTypeName = MissTypeName(missType);
            hid_t type_group = H5Gcreate2(pc_group, missTypeName, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            
            if (type_group < 0) {
                warn("[%s] Failed to create miss type group: %s", cacheName, missTypeName);
                continue;
            }
            
            // Write records for this miss type
            size_t numRecords = typeRecords.size();
            hsize_t record_dims[1] = {numRecords};
            hid_t record_space = H5Screate_simple(1, record_dims, NULL);
            hid_t cycle_type = H5Tcopy(H5T_NATIVE_UINT64);
            hid_t address_type = H5Tcopy(H5T_NATIVE_UINT64);
            
            // Extract cycles and addresses
            std::vector<uint64_t> cycles;
            std::vector<uint64_t> addresses;
            cycles.reserve(numRecords);
            addresses.reserve(numRecords);
            
            for (const auto& record : typeRecords) {
                cycles.push_back(record.cycle);
                addresses.push_back(record.address);
            }
            
            // Write cycles
            hid_t cycles_dataset = H5Dcreate2(type_group, "cycle", cycle_type, record_space,
                                            H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            H5Dwrite(cycles_dataset, cycle_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, cycles.data());
            H5Dclose(cycles_dataset);
            
            // Write addresses
            hid_t addresses_dataset = H5Dcreate2(type_group, "address", address_type, record_space,
                                               H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
            H5Dwrite(addresses_dataset, address_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, addresses.data());
            H5Dclose(addresses_dataset);
            
            H5Sclose(record_space);
            H5Tclose(cycle_type);
            H5Tclose(address_type);
            H5Gclose(type_group);
        }
        
        H5Gclose(pc_group);
    }
    
    H5Gclose(cache_group);
    H5Fclose(file_id);
    
    info("[%s] Exported %lu cache miss records (%lu unique PCs) to %s", 
         cacheName, totalRecords, pcToRecords.size(), filename);
    
    futex_unlock(&recorderLock);
}

// CacheMissRecorderManager implementation
CacheMissRecorderManager::CacheMissRecorderManager() 
    : globalEnabled(false), globalMaxRecords(0) {
    futex_init(&managerLock);
}

CacheMissRecorderManager::~CacheMissRecorderManager() {
    futex_lock(&managerLock);
    for (auto& pair : recorders) {
        delete pair.second;
    }
    recorders.clear();
    futex_unlock(&managerLock);
}

void CacheMissRecorderManager::init(bool enabled, uint64_t maxRecords) {
    futex_lock(&managerLock);
    globalEnabled = enabled;
    globalMaxRecords = maxRecords;
    futex_unlock(&managerLock);
    
    if (enabled) {
        info("Cache miss recording enabled with max %lu records per cache", maxRecords);
    } else {
        info("Cache miss recording disabled");
    }
}

CacheMissRecorder* CacheMissRecorderManager::getRecorder(const char* cacheName) {
    if (!globalEnabled) {
        return nullptr;
    }
    
    futex_lock(&managerLock);
    
    std::string cacheNameStr(cacheName);
    auto it = recorders.find(cacheNameStr);
    
    CacheMissRecorder* recorder = nullptr;
    if (it != recorders.end()) {
        recorder = it->second;
    } else {
        // Create new recorder
        recorder = new CacheMissRecorder(cacheName, globalMaxRecords, globalEnabled);
        recorders[cacheNameStr] = recorder;
        info("Created cache miss recorder for cache: %s", cacheName);
    }
    
    futex_unlock(&managerLock);
    return recorder;
}

void CacheMissRecorderManager::exportAllToHDF5(const char* outputDir) const {
    if (!globalEnabled || recorders.empty()) {
        return;
    }
    
    futex_lock(&managerLock);
    
    for (const auto& pair : recorders) {
        const std::string& cacheName = pair.first;
        CacheMissRecorder* recorder = pair.second;
        
        // Create filename
        std::string filename = std::string(outputDir) + "/cache_miss_records_" + cacheName + ".h5";
        
        // Export this recorder's data
        recorder->exportToHDF5(filename.c_str());
    }
    
    futex_unlock(&managerLock);
}

uint64_t CacheMissRecorderManager::getTotalRecordsAcrossAllCaches() const {
    uint64_t total = 0;
    
    futex_lock(&managerLock);
    for (const auto& pair : recorders) {
        total += pair.second->getTotalRecords();
    }
    futex_unlock(&managerLock);
    
    return total;
} 