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

#ifndef FILTER_CACHE_H_
#define FILTER_CACHE_H_

#include "bithacks.h"
#include "cache.h"
#include "galloc.h"
#include "zsim.h"
// modified by wei wu on 251211: Added PC access recorder include
#include "pc_access_recorder.h"

extern int sw_prefetch_count;
extern int movaps_ins_count;

/* Extends Cache with an L0 direct-mapped cache, optimized to hell for hits
 *
 * L1 lookups are dominated by several kinds of overhead (grab the cache locks,
 * several virtual functions for the replacement policy, etc.). This
 * specialization of Cache solves these issues by having a filter array that
 * holds the most recently used line in each set. Accesses check the filter array,
 * and then go through the normal access path. Because there is one line per set,
 * it is fine to do this without grabbing a lock.
 */

class FilterCache : public Cache {
    private:
        struct FilterEntry {
            volatile Address rdAddr;
            volatile Address wrAddr;
            volatile uint64_t availCycle;

            void clear() {wrAddr = 0; rdAddr = 0; availCycle = 0;}
        };

        //Replicates the most accessed line of each set in the cache
        FilterEntry* filterArray;
        Address setMask;
        uint32_t numSets;
        uint32_t srcId; //should match the core
        uint32_t reqFlags;

        lock_t filterLock;
        uint64_t fGETSHit, fGETXHit;
        
        // modified by wei wu on 251211: PC access recorder
        PCAccessRecorder* pcAccessRecorder;

    public:
        FilterCache(uint32_t _numSets, uint32_t _numLines, CC* _cc, CacheArray* _array,
                ReplPolicy* _rp, uint32_t _accLat, uint32_t _invLat, g_string& _name)
            : Cache(_numLines, _cc, _array, _rp, _accLat, _invLat, _name)
        {
            numSets = _numSets;
            setMask = numSets - 1;
            filterArray = gm_memalign<FilterEntry>(CACHE_LINE_BYTES, numSets);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            futex_init(&filterLock);
            fGETSHit = fGETXHit = 0;
            srcId = -1;
            reqFlags = 0;
            
            // modified by wei wu on 251211: Initialize PC access recorder (only for L1 caches)
            pcAccessRecorder = nullptr;
            if (g_pcAccessRecorderManager) {
                // Only create PC access recorder for L1 caches (FilterCache is typically used for L1)
                pcAccessRecorder = g_pcAccessRecorderManager->getRecorder(_name.c_str());
                if (pcAccessRecorder) {
                    info("[%s] PC access recorder initialized successfully (L1 cache)", _name.c_str());
                } else {
                    info("[%s] PC access recorder not created (recording disabled)", _name.c_str());
                }
            } else {
                info("[%s] Global PC access recorder manager not initialized", _name.c_str());
            }
        }

        void setSourceId(uint32_t id) {
            srcId = id;
        }

        void setFlags(uint32_t flags) {
            reqFlags = flags;
        }

        void initStats(AggregateStat* parentStat) {
            AggregateStat* cacheStat = new AggregateStat();
            cacheStat->init(name.c_str(), "Filter cache stats");

            ProxyStat* fgetsStat = new ProxyStat();
            fgetsStat->init("fhGETS", "Filtered GETS hits", &fGETSHit);
            ProxyStat* fgetxStat = new ProxyStat();
            fgetxStat->init("fhGETX", "Filtered GETX hits", &fGETXHit);
            cacheStat->append(fgetsStat);
            cacheStat->append(fgetxStat);

            initCacheStats(cacheStat);
            parentStat->append(cacheStat);
        }

        inline uint64_t load(Address vAddr, uint64_t curCycle, Address pc) {
            // modified by wei wu on 251211: Record PC access for load
            if (pcAccessRecorder) {
                pcAccessRecorder->recordAccess(pc, true);  // true for read access
            }
            
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races
            if (vLineAddr == filterArray[idx].rdAddr) {
                fGETSHit++;
                return MAX(curCycle, availCycle);
            } else {                
                return replace(vLineAddr, idx, true, curCycle, pc);
            }
        }

        inline uint64_t store(Address vAddr, uint64_t curCycle, Address pc) {
            // modified by wei wu on 251211: Record PC access for store
            if (pcAccessRecorder) {
                pcAccessRecorder->recordAccess(pc, false);  // false for write access
            }
            
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            uint64_t availCycle = filterArray[idx].availCycle; //read before, careful with ordering to avoid timing races

#ifdef PROFILING_MEMORY_FOOTPRINT
            if(pc== 0x4072d0||pc ==0x4072c0 || pc ==0x407400) {info("%dth iteration starting...................................................", movaps_ins_count);}
#endif

            if (vLineAddr == filterArray[idx].wrAddr) {
                fGETXHit++;
#ifdef PROFILING_MEMORY_FOOTPRINT
                if(pc == 0x4072e6 || pc == 0x4072e2 || pc == 0x4072d0 || pc == 0x4072d4 || pc == 0x4072d9 || pc == 0x4072de || pc == 0x4072e3||pc==0x4072ce||pc ==0x407400) {
                    info("fhGETX of 0x%lx, idx: %d, issue cycle: %ld, access_latency: %ld", pc, idx, curCycle, curCycle>=availCycle? 0 : availCycle-curCycle);
                    if(pc==0x4072e3||pc==0x4072e6||pc==0x4072ce||pc ==0x407400) {
                        movaps_ins_count++;
                    }
                }
#endif       
                //NOTE: Stores don't modify availCycle; we'll catch matches in the core
                //filterArray[idx].availCycle = curCycle; //do optimistic store-load forwarding
                return MAX(curCycle, availCycle);
            } else {
                if(vLineAddr == filterArray[idx].rdAddr) { //modified by wei on 251010: if the StoreAddr is the same as the RdAddr, we need to make sure the respCycle is not less than the availCycle
                    return MAX(replace(vLineAddr, idx, false, curCycle, pc), availCycle);
                }
                return replace(vLineAddr, idx, false, curCycle, pc);
            }
        }

        // modified by wei on 251010: add async software prefetch support
        inline void asyncSwPrefetch(Address vAddr, uint64_t curCycle, Address pc) {
            Address vLineAddr = vAddr >> lineBits;
            uint32_t idx = vLineAddr & setMask;
            
            // Check if already in cache
            if (vLineAddr == filterArray[idx].rdAddr) {
                return; // Already in cache, no need to prefetch
            }
            
            // Issue async prefetch request without waiting for response
            Address pLineAddr = procMask | vLineAddr;
            MESIState dummyState = MESIState::I;
            
            // Create software prefetch request with SW_PREFETCH flag
            MemReq req = {pLineAddr, GETS, 0, &dummyState, curCycle, nullptr, 
                          dummyState, srcId, MemReq::SW_PREFETCH, pc};
            
            // Issue async request - don't wait for response
            access(req);
        }

        uint64_t replace(Address vLineAddr, uint32_t idx, bool isLoad, uint64_t curCycle, Address pc) {
            Address pLineAddr = procMask | vLineAddr;
            MESIState dummyState = MESIState::I;
            futex_lock(&filterLock);
            MemReq req = {pLineAddr, isLoad? GETS : GETX, 0, &dummyState, curCycle, &filterLock, dummyState, srcId, reqFlags, pc};
            uint64_t respCycle  = access(req);

#ifdef PROFILING_MEMORY_FOOTPRINT
            if((pc == 0x4072d3||pc==0x4072c3) && isLoad) {
                info("%dth SW_Prefetch of 0x%lx, to idx: %d, issue cycle: %ld, access_latency: %ld", sw_prefetch_count, pc, idx, curCycle, respCycle-curCycle);
                sw_prefetch_count++;
            }
#endif
            //Due to the way we do the locking, at this point the old address might be invalidated, but we have the new address guaranteed until we release the lock

            //Careful with this order
            Address oldAddr = filterArray[idx].rdAddr;
            filterArray[idx].wrAddr = isLoad? -1L : vLineAddr; 
            filterArray[idx].rdAddr = vLineAddr;

            //For LSU simulation purposes, loads bypass stores even to the same line if there is no conflict,
            //(e.g., st to x, ld from x+8) and we implement store-load forwarding at the core.
            //So if this is a load, it always sets availCycle; if it is a store hit, it doesn't
            
#ifdef PROFILING_MEMORY_FOOTPRINT
            if (idx==223){
                info("update filter cache by pc: 0x%lx, access type: %s, idx: %d, availCycle: %ld, respCycle: %ld", pc, isLoad? "load" : "store", idx, filterArray[idx].availCycle, respCycle);
                info("rdAddr: 0x%lx, wrAddr: 0x%lx", filterArray[idx].rdAddr, filterArray[idx].wrAddr);
            }
#endif
            if (oldAddr != vLineAddr) filterArray[idx].availCycle = respCycle;

            futex_unlock(&filterLock);
            return respCycle;
        }

        uint64_t invalidate(const InvReq& req) {
            Cache::startInvalidate();  // grabs cache's downLock
            futex_lock(&filterLock);
            uint32_t idx = req.lineAddr & setMask; //works because of how virtual<->physical is done...
            if ((filterArray[idx].rdAddr | procMask) == req.lineAddr) { //FIXME: If another process calls invalidate(), procMask will not match even though we may be doing a capacity-induced invalidation!
                filterArray[idx].wrAddr = -1L;
                filterArray[idx].rdAddr = -1L;
            }
            uint64_t respCycle = Cache::finishInvalidate(req); // releases cache's downLock
            futex_unlock(&filterLock);
            return respCycle;
        }

        void contextSwitch() {
            futex_lock(&filterLock);
            for (uint32_t i = 0; i < numSets; i++) filterArray[i].clear();
            futex_unlock(&filterLock);
        }
};

#endif  // FILTER_CACHE_H_
