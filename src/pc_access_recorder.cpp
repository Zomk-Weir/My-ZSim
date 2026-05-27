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

#include "pc_access_recorder.h"
#include <hdf5.h>
#include <hdf5_hl.h>
#include <string>
#include <cstring>
#include "log.h"
#include "zsim.h"

// Global instance
PCAccessRecorderManager* g_pcAccessRecorderManager = nullptr;

// PCAccessRecorder implementation
PCAccessRecorder::PCAccessRecorder(const char* name, uint64_t maxRecs, bool enable) 
    : maxRecords(maxRecs), totalAccesses(0), enabled(enable) {
    cacheName = gm_strdup(name);
    futex_init(&recorderLock);
}

PCAccessRecorder::~PCAccessRecorder() {
    if (cacheName) {
        gm_free((void*)cacheName);
    }
}

void PCAccessRecorder::recordAccess(Address pc, bool isRead) {
    if (!enabled) {
        return;
    }
    
    futex_lock(&recorderLock);
    
    // Check if we've reached the maximum number of unique PCs
    if (pcToStats.size() >= maxRecords && pcToStats.find(pc) == pcToStats.end()) {
        if (pcToStats.size() == maxRecords) {
            info("PC Access Recorder: Reached max unique PCs limit (%lu), stopping new PC recording", maxRecords);
        }
        futex_unlock(&recorderLock);
        return;
    }
    
    // Record the access
    pcToStats[pc].recordAccess(isRead);
    totalAccesses++;
    
    // Debug info for first few accesses
    if (totalAccesses <= 10) {
        info("[%s] PC Access Recorder: Recorded access #%lu: PC=0x%lx, type=%s", 
             cacheName, totalAccesses, pc, isRead ? "READ" : "WRITE");
    }
    
    futex_unlock(&recorderLock);
}

void PCAccessRecorder::clear() {
    futex_lock(&recorderLock);
    pcToStats.clear();
    totalAccesses = 0;
    futex_unlock(&recorderLock);
}

std::vector<Address> PCAccessRecorder::getAllPCs() const {
    std::vector<Address> pcs;
    futex_lock(&recorderLock);
    pcs.reserve(pcToStats.size());
    for (const auto& pair : pcToStats) {
        pcs.push_back(pair.first);
    }
    futex_unlock(&recorderLock);
    return pcs;
}

const PCAccessStats* PCAccessRecorder::getStatsForPC(Address pc) const {
    futex_lock(&recorderLock);
    auto it = pcToStats.find(pc);
    const PCAccessStats* result = (it != pcToStats.end()) ? &it->second : nullptr;
    futex_unlock(&recorderLock);
    return result;
}

void PCAccessRecorder::exportToHDF5(const char* filename) const {
    if (totalAccesses == 0) {
        info("[%s] PC Access Recorder: No access records to export", cacheName);
        return;
    }
    
    futex_lock(&recorderLock);
    
    // Create HDF5 file
    hid_t file_id = H5Fcreate(filename, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
    if (file_id < 0) {
        warn("[%s] PC Access Recorder: Failed to create HDF5 file: %s", cacheName, filename);
        futex_unlock(&recorderLock);
        return;
    }
    
    // Create main group
    hid_t main_group = H5Gcreate2(file_id, "pc_access_stats", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (main_group < 0) {
        warn("PC Access Recorder: Failed to create main group in HDF5 file");
        H5Fclose(file_id);
        futex_unlock(&recorderLock);
        return;
    }
    
    // Write metadata
    hsize_t scalar_dims[1] = {1};
    hid_t scalar_space = H5Screate_simple(1, scalar_dims, NULL);
    hid_t uint64_type = H5Tcopy(H5T_NATIVE_UINT64);
    
    // Total accesses
    hid_t total_accesses_dataset = H5Dcreate2(main_group, "total_accesses", uint64_type, 
                                           scalar_space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Dwrite(total_accesses_dataset, uint64_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &totalAccesses);
    H5Dclose(total_accesses_dataset);
    
    // Unique PCs
    uint64_t uniquePCs = pcToStats.size();
    hid_t unique_pcs_dataset = H5Dcreate2(main_group, "unique_pcs", uint64_type, 
                                        scalar_space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Dwrite(unique_pcs_dataset, uint64_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, &uniquePCs);
    H5Dclose(unique_pcs_dataset);
    
    H5Sclose(scalar_space);
    H5Tclose(uint64_type);
    
    // Prepare arrays for bulk data export
    size_t numPCs = pcToStats.size();
    std::vector<uint64_t> pcAddresses;
    std::vector<uint64_t> totalAccessCounts;
    std::vector<uint64_t> readAccessCounts;
    std::vector<uint64_t> writeAccessCounts;
    
    pcAddresses.reserve(numPCs);
    totalAccessCounts.reserve(numPCs);
    readAccessCounts.reserve(numPCs);
    writeAccessCounts.reserve(numPCs);
    
    for (const auto& entry : pcToStats) {
        pcAddresses.push_back(entry.first);
        totalAccessCounts.push_back(entry.second.totalAccesses);
        readAccessCounts.push_back(entry.second.readAccesses);
        writeAccessCounts.push_back(entry.second.writeAccesses);
    }
    
    // Create datasets for bulk data
    hsize_t pc_dims[1] = {numPCs};
    hid_t pc_space = H5Screate_simple(1, pc_dims, NULL);
    hid_t pc_type = H5Tcopy(H5T_NATIVE_UINT64);
    
    // PC addresses
    hid_t pc_dataset = H5Dcreate2(main_group, "pc_addresses", pc_type, pc_space,
                                H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Dwrite(pc_dataset, pc_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, pcAddresses.data());
    H5Dclose(pc_dataset);
    
    // Total access counts
    hid_t total_dataset = H5Dcreate2(main_group, "total_access_counts", pc_type, pc_space,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Dwrite(total_dataset, pc_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, totalAccessCounts.data());
    H5Dclose(total_dataset);
    
    // Read access counts
    hid_t read_dataset = H5Dcreate2(main_group, "read_access_counts", pc_type, pc_space,
                                  H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Dwrite(read_dataset, pc_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, readAccessCounts.data());
    H5Dclose(read_dataset);
    
    // Write access counts
    hid_t write_dataset = H5Dcreate2(main_group, "write_access_counts", pc_type, pc_space,
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    H5Dwrite(write_dataset, pc_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, writeAccessCounts.data());
    H5Dclose(write_dataset);
    
    H5Sclose(pc_space);
    H5Tclose(pc_type);
    H5Gclose(main_group);
    H5Fclose(file_id);
    
    info("[%s] PC Access Recorder: Exported %lu access records (%lu unique PCs) to %s", 
         cacheName, totalAccesses, pcToStats.size(), filename);
    
    futex_unlock(&recorderLock);
}

// PCAccessRecorderManager implementation
PCAccessRecorderManager::PCAccessRecorderManager() 
    : globalEnabled(false), globalMaxRecords(0) {
    futex_init(&managerLock);
}

PCAccessRecorderManager::~PCAccessRecorderManager() {
    futex_lock(&managerLock);
    for (auto& pair : recorders) {
        delete pair.second;
    }
    recorders.clear();
    futex_unlock(&managerLock);
}

void PCAccessRecorderManager::init(bool enabled, uint64_t maxRecords) {
    futex_lock(&managerLock);
    globalEnabled = enabled;
    globalMaxRecords = maxRecords;
    futex_unlock(&managerLock);
    
    if (enabled) {
        info("PC Access Recorder: Initialized with max %lu unique PCs per cache", maxRecords);
    } else {
        info("PC Access Recorder: Disabled");
    }
}

PCAccessRecorder* PCAccessRecorderManager::getRecorder(const char* cacheName) {
    if (!globalEnabled) {
        return nullptr;
    }
    
    futex_lock(&managerLock);
    
    std::string cacheNameStr(cacheName);
    auto it = recorders.find(cacheNameStr);
    
    PCAccessRecorder* recorder = nullptr;
    if (it != recorders.end()) {
        recorder = it->second;
    } else {
        // Create new recorder
        recorder = new PCAccessRecorder(cacheName, globalMaxRecords, globalEnabled);
        recorders[cacheNameStr] = recorder;
        info("Created PC access recorder for cache: %s", cacheName);
    }
    
    futex_unlock(&managerLock);
    return recorder;
}

void PCAccessRecorderManager::exportAllToHDF5(const char* outputDir) const {
    if (!globalEnabled || recorders.empty()) {
        return;
    }
    
    futex_lock(&managerLock);
    
    for (const auto& pair : recorders) {
        const std::string& cacheName = pair.first;
        PCAccessRecorder* recorder = pair.second;
        
        // Create filename
        std::string filename = std::string(outputDir) + "/pc_access_records_" + cacheName + ".h5";
        
        // Export this recorder's data
        recorder->exportToHDF5(filename.c_str());
    }
    
    futex_unlock(&managerLock);
}

uint64_t PCAccessRecorderManager::getTotalAccessesAcrossAllCaches() const {
    uint64_t total = 0;
    
    futex_lock(&managerLock);
    for (const auto& pair : recorders) {
        total += pair.second->getTotalAccesses();
    }
    futex_unlock(&managerLock);
    
    return total;
}
