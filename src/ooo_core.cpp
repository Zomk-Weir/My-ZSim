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

#include <iostream>
#include "ooo_core.h"
#include <algorithm>
#include <queue>
#include <string>
#include "bithacks.h"
#include "decoder.h"
#include "filter_cache.h"
#include "zsim.h"

int sw_prefetch_count = 0; //for debugging the software prefetching, 0: first prefetch, 1: second prefetch, modified by wei on 251126
int movaps_ins_count = 0; //for debugging the movaps instruction, 0: first movaps, 1: second movaps, modified by wei on 251126

// ---- Targeted PC dispatch/commit cycle debug tracking ----
// Add the x86 instruction PCs you want to monitor (64-bit hex values).
// Set NUM_TRACKED_PCS to 0 (or leave the array empty) to disable tracking entirely.
// Example: static const uint64_t TRACKED_PCS[] = {0x401a20, 0x401b44};
static const uint64_t TRACKED_PCS[] = { // <-- replace with the PC(s) you want to track
    0x406000,
    0x406090,
    0x406073,
    0x4060f3,
    0x40612b,
    // 0x406183, //imul instruction, for testing the imul instruction's 1-many uops decoding
};
static const uint32_t NUM_TRACKED_PCS = sizeof(TRACKED_PCS) / sizeof(TRACKED_PCS[0]);

static inline bool isTrackedPC(uint64_t pc) {
    for (uint32_t k = 0; k < NUM_TRACKED_PCS; k++) {
        if (TRACKED_PCS[k] == pc) return true;
    }
    return false;
}

/* Uncomment to induce backpressure to the IW when the load/store buffers fill up. In theory, more detailed,
 * but sometimes much slower (as it relies on range poisoning in the IW, potentially O(n^2)), and in practice
 * makes a negligible difference (ROB backpressures).
 */
#define LSU_IW_BACKPRESSURE  //if you want to enable the LSU_IW_BACKPRESSURE, uncomment this line, and then you will observe that load/store request will be blocked when the load/store queue is full.

#define DEBUG_MSG(args...)
//#define DEBUG_MSG(args...) info(args)

// Core parameters
// TODO(dsm): Make OOOCore templated, subsuming these

// Stages --- more or less matched to Westmere, but have not seen detailed pipe diagrams anywhare
#define FETCH_STAGE 1
#define DECODE_STAGE 4  // NOTE: Decoder adds predecode delays to decode
#define ISSUE_STAGE 7
#define DISPATCH_STAGE 13  // RAT + ROB + RS, each is easily 2 cycles

// #define FETCH_STAGE 1
// #define DECODE_STAGE 2  // NOTE: Decoder adds predecode delays to decode
// #define ISSUE_STAGE 3
// #define DISPATCH_STAGE 4  // RAT + ROB + RS, each is easily 2 cycles

#define L1D_LAT 4  // fixed, and FilterCache does not include L1 delay
#define FETCH_BYTES_PER_CYCLE 16 //modified by wei on 251112: to expand the fetch bytes per cycle,initially set to 16
#define ISSUES_PER_CYCLE 4 //modified by wei on 251112: to expand the issue width,initially set to 4
#define RF_READS_PER_CYCLE 3 //modified by wei on 251112: to expand the RF reads per cycle,initially set to 3

int access_order = 0; //for debugging the software prefetching, 0: first access, 1: second access, modified by wei on 251202
#define DEBUG_ACCESS_ORDER_THRESHOLD 1

//uncomment next line to enable the profiling of the load store queue full, modified by wei on 251203
// #define PROFILING_LSQ_FULL
int lsq_full_count = 0; //for debugging the load store queue full, modified by wei on 251202
int lsq_full_cycles_sum = 0; //for debugging the load store queue full cycles sum, modified by wei on 251202
int lsq_full_cycles = 0; //for debugging the load store queue full cycles, modified by wei on 251202

OOOCore::OOOCore(FilterCache* _l1i, FilterCache* _l1d, g_string& _name) : Core(_name), l1i(_l1i), l1d(_l1d), cRec(0, _name) {
    decodeCycle = DECODE_STAGE;  // allow subtracting from it
    curCycle = 0;
    phaseEndCycle = zinfo->phaseLength;

    for (uint32_t i = 0; i < MAX_REGISTERS; i++) {
        regScoreboard[i] = 0;
    }
    prevBbl = nullptr;

    lastStoreCommitCycle = 0;
    lastStoreAddrCommitCycle = 0;
    curCycleRFReads = 0;
    curCycleIssuedUops = 0;
    branchPc = 0;

    LineChkDispatchCycle = 0;
    LineChkCommitCycle = 0;
    pcRangeActive = false;

    // Open CSV file for PC-range cycle tracking.
    // Each core writes to its own file: "<core_name>_pc_track.csv"
    if (NUM_TRACKED_PCS > 0){
        char csvPath[256];
        snprintf(csvPath, sizeof(csvPath), "%s_pc_track.csv", name.c_str());
        pcTrackCsvFile = fopen(csvPath, "w");
        if (pcTrackCsvFile) {
            // Use line-buffered mode so each fprintf flushes immediately
            setvbuf(pcTrackCsvFile, nullptr, _IOLBF, 0);
            fprintf(pcTrackCsvFile, "event_type,PC,dispatch_cycle,commit_cycle\n");
        } else {
            warn("OOOCore %s: failed to open %s for PC tracking CSV output", name.c_str(), csvPath);
        }
    }

    instrs = uops = bbls = approxInstrs = mispredBranches = BranchCount = 0;

    for (uint32_t i = 0; i < FWD_ENTRIES; i++) fwdArray[i].set((Address)(-1L), 0);
}

OOOCore::~OOOCore() {
    if (pcTrackCsvFile) {
        fflush(pcTrackCsvFile);
        fclose(pcTrackCsvFile);
        pcTrackCsvFile = nullptr;
    }
}

void OOOCore::initStats(AggregateStat* parentStat) {
    AggregateStat* coreStat = new AggregateStat();
    coreStat->init(name.c_str(), "Core stats");

    auto x = [this]() { return cRec.getUnhaltedCycles(curCycle); };
    LambdaStat<decltype(x)>* cyclesStat = new LambdaStat<decltype(x)>(x);
    cyclesStat->init("cycles", "Simulated unhalted cycles");

    auto y = [this]() { return cRec.getContentionCycles(); };
    LambdaStat<decltype(y)>* cCyclesStat = new LambdaStat<decltype(y)>(y);
    cCyclesStat->init("cCycles", "Cycles due to contention stalls");

    ProxyStat* instrsStat = new ProxyStat();
    instrsStat->init("instrs", "Simulated instructions", &instrs);
    ProxyStat* uopsStat = new ProxyStat();
    uopsStat->init("uops", "Retired micro-ops", &uops);
    ProxyStat* bblsStat = new ProxyStat();
    bblsStat->init("bbls", "Basic blocks", &bbls);
    ProxyStat* approxInstrsStat = new ProxyStat();
    approxInstrsStat->init("approxInstrs", "Instrs with approx uop decoding", &approxInstrs);
    ProxyStat* mispredBranchesStat = new ProxyStat();
    mispredBranchesStat->init("mispredBranches", "Mispredicted branches", &mispredBranches);
    ProxyStat* BranchCountStat = new ProxyStat();
    BranchCountStat->init("BranchCount", "Branch count", &BranchCount);

    coreStat->append(cyclesStat);
    coreStat->append(cCyclesStat);
    coreStat->append(instrsStat);
    coreStat->append(uopsStat);
    coreStat->append(bblsStat);
    coreStat->append(approxInstrsStat);
    coreStat->append(mispredBranchesStat);
    coreStat->append(BranchCountStat);

#ifdef OOO_STALL_STATS
    profFetchStalls.init("fetchStalls",  "Fetch stalls");  coreStat->append(&profFetchStalls);
    profDecodeStalls.init("decodeStalls", "Decode stalls"); coreStat->append(&profDecodeStalls);
    profIssueStalls.init("issueStalls",  "Issue stalls");  coreStat->append(&profIssueStalls);
#endif

    parentStat->append(coreStat);
}

uint64_t OOOCore::getInstrs() const {return instrs;}
uint64_t OOOCore::getPhaseCycles() const {return curCycle % zinfo->phaseLength;}

void OOOCore::contextSwitch(int32_t gid) {
    if (gid == -1) {
        // Do not execute previous BBL, as we were context-switched
        prevBbl = nullptr;

        // Invalidate virtually-addressed filter caches
        l1i->contextSwitch();
        l1d->contextSwitch();
    }
}


InstrFuncPtrs OOOCore::GetFuncPtrs() {return {LoadFunc, StoreFunc, BblFunc, BranchFunc, PredLoadFunc, PredStoreFunc, FPTR_ANALYSIS, {0}};}

inline void OOOCore::load(Address addr, Address pc) {
    loadAddrs[loads] = addr;
    loadPCs[loads] = pc;
    loads++;
}

void OOOCore::store(Address addr, Address pc) {
    storeAddrs[stores] = addr;
    storePCs[stores] = pc;
    stores++;
}

// Predicated loads and stores call this function, gets recorded as a 0-cycle op.
// Predication is rare enough that we don't need to model it perfectly to be accurate (i.e. the uops still execute, retire, etc), but this is needed for correctness.
void OOOCore::predFalseLoad() {
    loadAddrs[loads] = -1L;
    loadPCs[loads] = -1L;
    loads++;
}

void OOOCore::predFalseStore() {
    storeAddrs[stores] = -1L;
    storePCs[stores] = -1L;
    stores++;
}

void OOOCore::branch(Address pc, bool taken, Address takenNpc, Address notTakenNpc) {
    branchPc = pc;
    branchTaken = taken;
    branchTakenNpc = takenNpc;
    branchNotTakenNpc = notTakenNpc;
}

inline void OOOCore::bbl(Address bblAddr, BblInfo* bblInfo) {
    if (!prevBbl) {
        // This is the 1st BBL since scheduled, nothing to simulate
        prevBbl = bblInfo;
        // Kill lingering ops from previous BBL
        loads = stores = 0;
        return;
    }

    /* Simulate execution of previous BBL */

    uint32_t bblInstrs = prevBbl->instrs;
    DynBbl* bbl = &(prevBbl->oooBbl[0]);
    prevBbl = bblInfo;

    uint32_t loadIdx = 0;
    uint32_t storeIdx = 0;

    uint32_t prevDecCycle = 0;
    uint64_t lastCommitCycle = 0;  // used to find misprediction penalty

    // ---- Instruction-level PC tracking state (reset each BBL) ----
    // Same-instruction uops are always contiguous in uop[], so we detect
    // instruction boundaries by watching for PC changes as we iterate.
    uint64_t trackInstrPC = 0;       // PC of the instruction being accumulated; 0 = none
    uint64_t trackInstrDispatch = 0; // dispatchCycle of its first uop
    uint64_t trackInstrCommit = 0;   // commitCycle of its last uop so far
    // NOTE: LineChkDispatchCycle and LineChkCommitCycle are member variables (persist across BBL calls)

    // Run dispatch/IW
    for (uint32_t i = 0; i < bbl->uops; i++) {
        DynUop* uop = &(bbl->uop[i]);

        // Decode stalls
        uint32_t decDiff = uop->decCycle - prevDecCycle;
        decodeCycle = MAX(decodeCycle + decDiff, uopQueue.minAllocCycle());
        if (decodeCycle > curCycle) {
            // info("Decode stall %ld %ld | %d %d", decodeCycle, curCycle, uop->decCycle, prevDecCycle);
            uint32_t cdDiff = decodeCycle - curCycle;
            if(decodeCycle == uopQueue.minAllocCycle()) {
                info("Decode stall for %d cycles b/c of uopQueue", cdDiff);
            }
            else {
                info("Decode stall for %d cycles b/c of decDiff", cdDiff);
            }
#ifdef OOO_STALL_STATS
            profDecodeStalls.inc(cdDiff);
#endif
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
            for (uint32_t i = 0; i < cdDiff; i++) insWindow.advancePos(curCycle);
        }
        prevDecCycle = uop->decCycle;
        uopQueue.markLeave(curCycle); //mark the uop as left the uop queue, it means the uop is ready to be issued.

        // Implement issue width limit --- we can only issue 4 uops/cycle
        if (curCycleIssuedUops >= ISSUES_PER_CYCLE) {
#ifdef OOO_STALL_STATS
            profIssueStalls.inc();
#endif
            // info("Advancing due to uop issue width");
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
            insWindow.advancePos(curCycle);
        }
        curCycleIssuedUops++;

        // Kill dependences on invalid register
        // Using curCycle saves us two unpredictable branches in the RF read stalls code
        regScoreboard[0] = curCycle;

        uint64_t c0 = regScoreboard[uop->rs[0]];
        uint64_t c1 = regScoreboard[uop->rs[1]];

        // RF read stalls
        // if srcs are not available at issue time, we have to go thru the RF
        curCycleRFReads += ((c0 < curCycle)? 1 : 0) + ((c1 < curCycle)? 1 : 0);
        if (curCycleRFReads > RF_READS_PER_CYCLE) {
            curCycleRFReads -= RF_READS_PER_CYCLE;
            curCycleIssuedUops = 0;  // or 1? that's probably a 2nd-order detail
            insWindow.advancePos(curCycle);
        }

        uint64_t c2 = rob.minAllocCycle(); //the minimum cycle of the ROB, it means the earliest cycle that the uop can be dispatched.
        uint64_t c3 = curCycle; //the current cycle, it means the cycle that the uop is issued.
        if (c2 > c3) {
            // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "[ROB_full]\n");
        }
        

        uint64_t cOps = MAX(c0, c1); //to simulate the dependency between the two source registers, we need to use the maximum of the two source registers' commit cycles.

        // Model RAT + ROB + RS delay between issue and dispatch
        uint64_t dispatchCycle = MAX(cOps, MAX(c2, c3) + (DISPATCH_STAGE - ISSUE_STAGE));

        // info("IW 0x%lx %d %ld %ld %x", bblAddr, i, c2, dispatchCycle, uop->portMask);
        // NOTE: Schedule can adjust both cur and dispatch cycles
        insWindow.schedule(curCycle, dispatchCycle, uop->portMask, uop->extraSlots);

        // //track the diasired instruction's issue and dispatch cycle
        // if (NUM_TRACKED_PCS > 0) {
        //     const uint64_t* uopPCArr = bbl->uopPCArray();
        //     uint64_t uopPC = uopPCArr[i];
        //     if (uopPC == 0x406090) { //Fetch&Boundary_start
        //         fprintf(pcTrackCsvFile, "[F&B_start], I_C:%lu, Adv:%d, D_delay:%d\n", (uint64_t)c3,uint8_t(curCycle-c3), (uint8_t)(dispatchCycle-c3));
        //     }
        //     else if (uopPC == 0x4060f3){ //Fetch&Boundary_end
        //         fprintf(pcTrackCsvFile, "Fetch&Boundary_end\n");
        //     }
        //     else if (uopPC == 0x40612b){ //residual line check
        //         fprintf(pcTrackCsvFile, "residual line check\n");
        //     }

        //     //for profiling the uop counting
        //     // if (uopPC >= 0x406000 && uopPC <= 0x406073)
        //     // {
        //     //     fprintf(pcTrackCsvFile, "[Uop counting], PC:%lx, Uop counting... \n", uopPC);
        //     // }
            
        // }

        // If we have advanced, we need to reset the curCycle counters
        if (curCycle > c3) {
            curCycleIssuedUops = 0;
            curCycleRFReads = 0;
        }

        uint64_t commitCycle;

        // LSU simulation
        // NOTE: Ever-so-slightly faster than if-else if-else if-else
        switch (uop->type) {
            case UOP_GENERAL:
                commitCycle = dispatchCycle + uop->lat;
                if (access_order<DEBUG_ACCESS_ORDER_THRESHOLD){
                    info("genereal uop, commitCycle: %ld", commitCycle);
                }
                break;

            case UOP_LOAD:
                {
                    // dispatchCycle = MAX(loadQueue.minAllocCycle(), dispatchCycle);
                    uint64_t lqCycle = loadQueue.minAllocCycle();
                    if (lqCycle > dispatchCycle) {
                        // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "[load_queue_full]\n");
#ifdef PROFILING_LSQ_FULL
                        lsq_full_count++;
                        lsq_full_cycles = lqCycle - dispatchCycle;
                        lsq_full_cycles_sum += lsq_full_cycles;
                        info("load queue is full, lsq_full_count: %d, lsq_full_cycles: %d, lsq_full_cycles_sum: %d", lsq_full_count, lsq_full_cycles, lsq_full_cycles_sum);
                        
#endif
#ifdef LSU_IW_BACKPRESSURE
                        insWindow.poisonRange(curCycle, lqCycle, 0x4 /*PORT_2, loads*/);
#endif
                        dispatchCycle = lqCycle;
                    }

                    // Wait for all previous store addresses to be resolved
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

                    Address addr = loadAddrs[loadIdx];
                    Address pc = loadPCs[loadIdx];
                    loadIdx++;
                    uint64_t reqSatisfiedCycle = dispatchCycle;
                    if (addr != ((Address)-1L)) {
                        reqSatisfiedCycle = l1d->load(addr, dispatchCycle, pc) + L1D_LAT + uop->lat;
                        cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);
                    }

                    // Enforce st-ld forwarding
                    uint32_t fwdIdx = (addr>>2) & (FWD_ENTRIES-1);
                    if (fwdArray[fwdIdx].addr == addr) {
                        // info("0x%lx FWD %ld %ld", addr, reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                        /* Take the MAX (see FilterCache's code) Our fwdArray
                         * imposes more stringent timing constraints than the
                         * l1d, b/c FilterCache does not change the line's
                         * availCycle on a store. This allows FilterCache to
                         * track per-line, not per-word availCycles.
                         */
                        reqSatisfiedCycle = MAX(reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                    }

                    commitCycle = reqSatisfiedCycle;
                    loadQueue.markRetire(commitCycle);
                    if (access_order<DEBUG_ACCESS_ORDER_THRESHOLD){
                        info("the %dth access, type: load, commitCycle: %ld", access_order, commitCycle);
                        access_order++;
                    }
                }
                break;

            // modified by wei on 251010: add software prefetch handling
            case UOP_SW_PREFETCH:
                //the first version of software prefetch handling
                // {
                //     // Software prefetch: fast completion, async memory request
                //     Address addr = loadAddrs[loadIdx];
                //     Address pc = loadPCs[loadIdx];
                //     loadIdx++;
                    
                //     // Complete immediately without waiting for memory response
                //     commitCycle = dispatchCycle + uop->lat; // lat = 1, fast completion
                    
                //     // Issue async prefetch request if address is valid
                //     if (addr != ((Address)-1L)) {
                //         // Async software prefetch - don't wait for response
                //         l1d->asyncSwPrefetch(addr, dispatchCycle, pc);
                //     }
                // }

                //the second version of software prefetch handling
                {
                    // dispatchCycle = MAX(loadQueue.minAllocCycle(), dispatchCycle);
                    uint64_t lqCycle = loadQueue.minAllocCycle();
                    if (lqCycle > dispatchCycle) {  //if load queue is full, and we need to wait for the load queue to be available.
                        // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "[load_queue_full]\n");
#ifdef PROFILING_LSQ_FULL
                        lsq_full_count++;
                        lsq_full_cycles = lqCycle - dispatchCycle;
                        lsq_full_cycles_sum += lsq_full_cycles;
                        info("load queue is full, lsq_full_count: %d, lsq_full_cycles: %d, lsq_full_cycles_sum: %d", lsq_full_count, lsq_full_cycles, lsq_full_cycles_sum);
#endif
#ifdef LSU_IW_BACKPRESSURE
                        insWindow.poisonRange(curCycle, lqCycle, 0x4 /*PORT_2, loads*/);
#endif
                        dispatchCycle = lqCycle;
                    }

                    // Wait for all previous store addresses to be resolved
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

                    Address addr = loadAddrs[loadIdx];
                    Address pc = loadPCs[loadIdx];
                    loadIdx++;
                    // uint64_t reqSatisfiedCycle = dispatchCycle; //for timing cache type, it will cause assertion failure, so we don't use this variable.
                    if (addr != ((Address)-1L)) {
                        // reqSatisfiedCycle = l1d->load(addr, dispatchCycle, pc) + L1D_LAT;  //this is the load operation, we don't want to do this way.
                        //first, we "silently" complete the load operation.
                        l1d->load(addr, dispatchCycle, pc);
                        //second, we set the request satisfied cycle to the next cycle.
                        // reqSatisfiedCycle = dispatchCycle+1; //for timing cache type, it will cause assertion failure, so we don't use this variable.
                        //thirdly, not sure if this is correct, but it seems to be the only way to record the memory access when the software prefetch is issued
                        // cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle); //in timing cache type, it will cause assertion failure, so we don't record it.
                        //cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle); //do we need to record the memory access when the software prefetch is issued? seems have no effect on the cycle count
                    }
                    
                    //let's ignore the st-ld forwarding for now 20251118 b/c reqSatisfiedCycle is set to the dispatch cycle.
                    // // Enforce st-ld forwarding from original load operation, let's keep the original load operation for now.
                    // uint32_t fwdIdx = (addr>>2) & (FWD_ENTRIES-1);
                    // if (fwdArray[fwdIdx].addr == addr) {
                    //     // info("0x%lx FWD %ld %ld", addr, reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                    //     /* Take the MAX (see FilterCache's code) Our fwdArray
                    //      * imposes more stringent timing constraints than the
                    //      * l1d, b/c FilterCache does not change the line's
                    //      * availCycle on a store. This allows FilterCache to
                    //      * track per-line, not per-word availCycles.
                    //      */
                    //     reqSatisfiedCycle = MAX(reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                    // }
                    // commitCycle = reqSatisfiedCycle;

                    
                    //commitCycle = dispatchCycle + uop->lat; // lat = 1, fast completion, the only difference is that we don't wait for the memory response
                    commitCycle = dispatchCycle+1;
                    loadQueue.markRetire(dispatchCycle);
                    if (access_order<DEBUG_ACCESS_ORDER_THRESHOLD){
                        info("the %dth access, type: sw_prefetch, commitCycle: %ld", access_order, commitCycle);
                        access_order++;
                    }
                }
                break;

            case UOP_OVEC:
                {
                    // dispatchCycle = MAX(loadQueue.minAllocCycle(), dispatchCycle);
                    uint64_t lqCycle = loadQueue.minAllocCycle();
                    if (lqCycle > dispatchCycle) {
#ifdef PROFILING_LSQ_FULL
                        lsq_full_count++;
                        lsq_full_cycles = lqCycle - dispatchCycle;
                        lsq_full_cycles_sum += lsq_full_cycles;
                        info("load queue is full, lsq_full_count: %d, lsq_full_cycles: %d, lsq_full_cycles_sum: %d", lsq_full_count, lsq_full_cycles, lsq_full_cycles_sum);
#endif
#ifdef LSU_IW_BACKPRESSURE
                        insWindow.poisonRange(curCycle, lqCycle, 0x4 /*PORT_2, loads*/);
#endif
                        dispatchCycle = lqCycle;
                    }

                    // Wait for all previous store addresses to be resolved
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

                    if (Decoder::ovecLoadAddresses.empty()) {
                        // Marged
                        commitCycle = lqCycle;
                        info("OVEC LOAD empty address, breaking.............."); //why so many ovec ins with empty address??
                        break;
                    }

                    Address addr = Decoder::ovecLoadAddresses.front();
                    Decoder::ovecLoadAddresses.pop();
                    info("OVEC poped address: %ld", addr);
                    Address pc = 0L;

                    uint64_t reqSatisfiedCycle = dispatchCycle;
                    if (addr == ((Address)0L)) {
                        commitCycle = lqCycle;  // Drop
                        info("OVEC LOAD zero address");
                        break;
                    } else if (addr != ((Address)-1L)) {
                        reqSatisfiedCycle = l1d->load(addr, dispatchCycle, pc) + L1D_LAT + 5 /*OVEC address generation latency (TODO: too pessimistic)*/;
                        info("OVEC LOAD pc: 0x%lx", pc);
                        cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);
                    }
                    info("OVEC Operation finished");

                    // Enforce st-ld forwarding
                    uint32_t fwdIdx = (addr>>2) & (FWD_ENTRIES-1);
                    if (fwdArray[fwdIdx].addr == addr) {
                        // info("0x%lx FWD %ld %ld", addr, reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                        /* Take the MAX (see FilterCache's code) Our fwdArray
                         * imposes more stringent timing constraints than the
                         * l1d, b/c FilterCache does not change the line's
                         * availCycle on a store. This allows FilterCache to
                         * track per-line, not per-word availCycles.
                         */
                        reqSatisfiedCycle = MAX(reqSatisfiedCycle, fwdArray[fwdIdx].storeCycle);
                    }

                    commitCycle = reqSatisfiedCycle;
                    loadQueue.markRetire(commitCycle);
                    if (access_order<DEBUG_ACCESS_ORDER_THRESHOLD){
                        info("the %dth access, type: ovec, commitCycle: %ld", access_order, commitCycle);
                        access_order++;
                    }
                }
                break;


            case UOP_STORE:
                {
                    // dispatchCycle = MAX(storeQueue.minAllocCycle(), dispatchCycle);
                    uint64_t sqCycle = storeQueue.minAllocCycle();
                    if (sqCycle > dispatchCycle) {
                        // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "[store_queue_full]\n");
#ifdef PROFILING_LSQ_FULL
                        lsq_full_count++;
                        lsq_full_cycles = sqCycle - dispatchCycle;
                        lsq_full_cycles_sum += lsq_full_cycles;
                        info("store queue is full, lsq_full_count: %d, lsq_full_cycles: %d, lsq_full_cycles_sum: %d", lsq_full_count, lsq_full_cycles, lsq_full_cycles_sum);
#endif
#ifdef LSU_IW_BACKPRESSURE
                        insWindow.poisonRange(curCycle, sqCycle, 0x10 /*PORT_4, stores*/);
#endif
                        dispatchCycle = sqCycle;
                    }

                    // Wait for all previous store addresses to be resolved (not just ours :))
                    dispatchCycle = MAX(lastStoreAddrCommitCycle+1, dispatchCycle);

                    Address addr = storeAddrs[storeIdx];
                    Address pc = storePCs[storeIdx];
                    storeIdx++;
                    uint64_t reqSatisfiedCycle = l1d->store(addr, dispatchCycle, pc) + L1D_LAT;
                    cRec.record(curCycle, dispatchCycle, reqSatisfiedCycle);

                    // Fill the forwarding table
                    fwdArray[(addr>>2) & (FWD_ENTRIES-1)].set(addr, reqSatisfiedCycle);

                    commitCycle = reqSatisfiedCycle;
                    lastStoreCommitCycle = MAX(lastStoreCommitCycle, reqSatisfiedCycle);
                    storeQueue.markRetire(commitCycle);

                    if (access_order<DEBUG_ACCESS_ORDER_THRESHOLD){
                        info("the %dth access, type: store, commitCycle: %ld", access_order, commitCycle);
                        access_order++;
                    }
                }
                break;

            case UOP_STORE_ADDR:
                commitCycle = dispatchCycle + uop->lat;
                lastStoreAddrCommitCycle = MAX(lastStoreAddrCommitCycle, commitCycle);
                if (access_order<DEBUG_ACCESS_ORDER_THRESHOLD){
                    info("the %dth access, type: store_addr, commitCycle: %ld", access_order, commitCycle);
                    access_order++;
                }
                break;

            //case UOP_FENCE:  //make gcc happy
            default:
                assert((UopType) uop->type == UOP_FENCE);
                commitCycle = dispatchCycle + uop->lat;
                // info("%d %ld %ld", uop->lat, lastStoreAddrCommitCycle, lastStoreCommitCycle);
                // force future load serialization
                lastStoreAddrCommitCycle = MAX(commitCycle, MAX(lastStoreAddrCommitCycle, lastStoreCommitCycle + uop->lat));
                // info("%d %ld %ld X", uop->lat, lastStoreAddrCommitCycle, lastStoreCommitCycle);
                if (access_order<DEBUG_ACCESS_ORDER_THRESHOLD){
                    info("the %dth access, type: default, commitCycle: %ld", access_order, commitCycle);
                    access_order++;
                }
        }

        // Mark retire at ROB
        rob.markRetire(commitCycle);

        if (access_order<DEBUG_ACCESS_ORDER_THRESHOLD){
            info("ROB info, commitCycle: %ld", commitCycle);
        }

        // Record dependences
        regScoreboard[uop->rd[0]] = commitCycle;
        regScoreboard[uop->rd[1]] = commitCycle;

        lastCommitCycle = commitCycle;

        // ---- Instruction-level dispatch/commit cycle tracking ----
        // When the PC changes, the previous instruction is complete: flush it.
        // Then record first-uop dispatch for the new instruction.
        // commitCycle keeps getting overwritten, so the last uop's value survives.
        // if (NUM_TRACKED_PCS > 0) {
        if (false){
            const uint64_t* uopPCArr = bbl->uopPCArray();
            uint64_t uopPC = uopPCArr[i];
            if (uopPC != trackInstrPC) {
                // New x86 instruction starting — flush the previous one if it was tracked
                // Also flush any instruction that falls inside the active range window.
                if (trackInstrPC != 0 && (isTrackedPC(trackInstrPC) || pcRangeActive)) {
                    // info("[PC_TRACK] PC=0x%lx dispatchCycle=%lu commitCycle=%lu",
                    //      trackInstrPC, trackInstrDispatch, trackInstrCommit);
                   
                    // the 1st line collision check PC tracking code
                    // if (trackInstrPC == 0x406000) {
                    //     LineChkDispatchCycle = trackInstrDispatch;
                    //     if (LineChkDispatchCycle < LineChkCommitCycle) {
                    //         uint64_t overlapCyc = LineChkCommitCycle - LineChkDispatchCycle;
                    //         info("Line Chk overlap %lu cycles", overlapCyc);
                    //         if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "overlap,%lu\n", overlapCyc);
                    //     }
                    // }
                    // if (trackInstrPC == 0x4060f3) {
                    //     LineChkCommitCycle = trackInstrCommit;
                    //     uint64_t consumedCyc = LineChkCommitCycle - LineChkDispatchCycle;
                    //     info("Line Chk consumed %lu cycles", consumedCyc);
                    //     if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "consumed,%lu\n", consumedCyc);
                    //     LineChkDispatchCycle = 0;
                    // }

                    //the 2nd line collision check PC tracking code
                    if (trackInstrPC == 0x406000) {
                        // info("[PC_TRACK] PC=0x%lx dispatchCycle=%lu commitCycle=%lu",
                        //      trackInstrPC, trackInstrDispatch, trackInstrCommit);
                        // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "Gather&Test_start,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);
                        
                    }
                    else if (trackInstrPC == 0x406073) {
                        // info("[Gather&Test_end],0x%lx,%lu,%lu", trackInstrPC, trackInstrDispatch, trackInstrCommit);
                        // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "Gather&Test_end,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);
                        
                    }
                    else if (pcRangeActive) {
                        // Instruction falls inside the [0x406090, 0x4060f3) window
                        // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "range,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);
                    }
                    else if (trackInstrPC == 0x406090) {
                        // info("[Fetch&Boundary_start],0x%lx,%lu,%lu", trackInstrPC, trackInstrDispatch, trackInstrCommit);
                        // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "Fetch&Boundary_start,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);
                        // Enable range tracking: record every instruction between here and 0x406090
                        pcRangeActive = false;

                        
                    }
                    else if (trackInstrPC == 0x4060f3) {
                        if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "Fetch&Boundary_end,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);

                    }
                    else if (trackInstrPC == 0x40612b) {
                        // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "residual line check,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);
                        
                        // fprintf(pcTrackCsvFile, "residual line check\n");
                    }
                }
                // Close range window the moment the 0x406090 instruction begins,
                // so 0x406090 itself is handled by its own "Fetch&Boundary_start" branch.
                if (uopPC == 0x406073) pcRangeActive = false;
                trackInstrPC = uopPC;
                // Record first uop's dispatchCycle if this PC is tracked or inside the range window
                trackInstrDispatch = (isTrackedPC(uopPC) || pcRangeActive) ? dispatchCycle : 0;
                trackInstrCommit   = 0;
            }
            // Always update: last uop of this instruction wins for commitCycle
            if (isTrackedPC(uopPC) || pcRangeActive) {
                trackInstrCommit = commitCycle;
            }
        }

        // info("0x%lx %3d [%3d %3d] -> [%3d %3d]  %8u %8lu %8lu %8lu", bbl->addr, i, uop->rs[0], uop->rs[1], uop->rd[0], uop->rd[1], uop->decCycle, c3, dispatchCycle, commitCycle);
    }

    // Flush the last instruction in this BBL (loop above only flushes on PC change,
    // so the very last instruction is still pending after the loop ends)
    if (NUM_TRACKED_PCS > 0 && trackInstrPC != 0 && (isTrackedPC(trackInstrPC) || pcRangeActive)) {
        if (trackInstrPC == 0x406000) {
            // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "Gather&Test_start,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);
            
        }
        else if (trackInstrPC == 0x406073) {
            // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "Gather&Test_end,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);
            
        }
        else if (pcRangeActive) {
            // Last instruction of this BBL is inside the [0x406090, 0x4060f3) window
            // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "range,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);
        }
        else if (trackInstrPC == 0x406090) {
            // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "Fetch&Boundary_start,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);
            // // Enable range tracking: record every instruction between here and 0x406090
            // pcRangeActive = false;

        }
        else if (trackInstrPC == 0x4060f3) {
            // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "Fetch&Boundary_end,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);

            // fprintf(pcTrackCsvFile, "Fetch&Boundary_end\n");
        }
        else if (trackInstrPC == 0x40612b) {
            // if (pcTrackCsvFile) fprintf(pcTrackCsvFile, "residual line check,0x%lx,%lu,%lu\n", trackInstrPC, trackInstrDispatch, trackInstrCommit);

        }
    }

    instrs += bblInstrs;
    uops += bbl->uops;
    bbls++;
    approxInstrs += bbl->approxInstrs;

#ifdef BBL_PROFILING
    if (approxInstrs) Decoder::profileBbl(bbl->bblIdx);
#endif

    // Check full match between expected and actual mem ops
    // If these assertions fail, most likely, something's off in the decoder
    assert_msg(loadIdx == loads, "%s: loadIdx(%d) != loads (%d)", name.c_str(), loadIdx, loads);
    assert_msg(storeIdx == stores, "%s: storeIdx(%d) != stores (%d)", name.c_str(), storeIdx, stores);
    loads = stores = 0;


    /* Simulate frontend for branch pred + fetch of this BBL
     *
     * NOTE: We assume that the instruction length predecoder and the IQ are
     * weak enough that they can't hide any ifetch or bpred stalls. In fact,
     * predecoder stalls are incorporated in the decode stall component (see
     * decoder.cpp). So here, we compute fetchCycle, then use it to adjust
     * decodeCycle.
     */

    // Model fetch-decode delay (fixed, weak predec/IQ assumption)
    uint64_t fetchCycle = decodeCycle - (DECODE_STAGE - FETCH_STAGE);
    uint32_t lineSize = 1 << lineBits;

    // Simulate branch 
    if (branchPc) { //if there is a branch, we need to simulate it
        BranchCount++;
        if (!branchPred.predict(branchPc, branchTaken)) {
            mispredBranches++;
    
            /* Simulate wrong-path fetches
             *
             * This is not for a latency reason, but sometimes it increases fetched
             * code footprint and L1I MPKI significantly. Also, we assume a perfect
             * BTB here: we always have the right address to missfetch on, and we
             * never need resteering.
             *
             * NOTE: Resteering due to BTB misses is done at the BAC unit, is
             * relatively rare, and carries an 8-cycle penalty, which should be
             * partially hidden if the branch is predicted correctly --- so we
             * don't simulate it.
             *
             * Since we don't have a BTB, we just assume the next branch is not
             * taken. With a typical branch mispred penalty of 17 cycles, we
             * typically fetch 3-4 lines in advance (16B/cycle). This sets a higher
             * limit, which can happen with branches that take a long time to
             * resolve (because e.g., they depend on a load). To set this upper
             * bound, assume a completely backpressured IQ (18 instrs), uop queue
             * (28 uops), IW (36 uops), and 16B instr length predecoder buffer. At
             * ~3.5 bytes/instr, 1.2 uops/instr, this is about 5 64-byte lines.
             */
    
            // info("Mispredicted branch, %ld %ld %ld | %ld %ld", decodeCycle, curCycle, lastCommitCycle,
            //         lastCommitCycle-decodeCycle, lastCommitCycle-curCycle);
            Address wrongPathAddr = branchTaken? branchNotTakenNpc : branchTakenNpc;
            uint64_t reqCycle = fetchCycle;
            for (uint32_t i = 0; i < 5*64/lineSize; i++) {
                uint64_t fetchLat = l1i->load(wrongPathAddr + lineSize*i, curCycle, wrongPathAddr + lineSize*i /*Kasraa: This is instruction cache and the PC is not required*/) - curCycle;
                cRec.record(curCycle, curCycle, curCycle + fetchLat);
                uint64_t respCycle = reqCycle + fetchLat;
                if (respCycle > lastCommitCycle) {
                    break;
                }
                // Model fetch throughput limit
                reqCycle = respCycle + lineSize/FETCH_BYTES_PER_CYCLE;
            }
    
            fetchCycle = lastCommitCycle;
        }
    }
    
    
    branchPc = 0;  // clear for next BBL

    // Simulate current bbl ifetch
    Address endAddr = bblAddr + bblInfo->bytes;
    for (Address fetchAddr = bblAddr; fetchAddr < endAddr; fetchAddr += lineSize) {
        // The Nehalem frontend fetches instructions in 16-byte-wide accesses.
        // Do not model fetch throughput limit here, decoder-generated stalls already include it
        // We always call fetches with curCycle to avoid upsetting the weave
        // models (but we could move to a fetch-centric recorder to avoid this)
        uint64_t fetchLat = l1i->load(fetchAddr, curCycle, fetchAddr /*Kasraa: This is instruction cache and the PC is not required*/) - curCycle;
     
        
        cRec.record(curCycle, curCycle, curCycle + fetchLat);
        fetchCycle += fetchLat;
    }

    // If fetch rules, take into account delay between fetch and decode;
    // If decode rules, different BBLs make the decoders skip a cycle
    decodeCycle++;
    uint64_t minFetchDecCycle = fetchCycle + (DECODE_STAGE - FETCH_STAGE);
    if (minFetchDecCycle > decodeCycle) {
#ifdef OOO_STALL_STATS
        profFetchStalls.inc(minFetchDecCycle - decodeCycle);
        // info("fetch stall: %ld cycles", minFetchDecCycle - decodeCycle);
#endif
        decodeCycle = minFetchDecCycle;
    }
}

// Timing simulation code
void OOOCore::join() {
    DEBUG_MSG("[%s] Joining, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    uint64_t targetCycle = cRec.notifyJoin(curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
    phaseEndCycle = zinfo->globPhaseCycles + zinfo->phaseLength;
    // assert(targetCycle <= phaseEndCycle);
    DEBUG_MSG("[%s] Joined, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
}

void OOOCore::leave() {
    DEBUG_MSG("[%s] Leaving, curCycle %ld phaseEnd %ld", name.c_str(), curCycle, phaseEndCycle);
    cRec.notifyLeave(curCycle);
}

void OOOCore::cSimStart() {
    uint64_t targetCycle = cRec.cSimStart(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

void OOOCore::cSimEnd() {
    uint64_t targetCycle = cRec.cSimEnd(curCycle);
    assert(targetCycle >= curCycle);
    if (targetCycle > curCycle) advance(targetCycle);
}

void OOOCore::advance(uint64_t targetCycle) {
    assert(targetCycle > curCycle);
    decodeCycle += targetCycle - curCycle;
    insWindow.longAdvance(curCycle, targetCycle);
    curCycleRFReads = 0;
    curCycleIssuedUops = 0;
    assert(targetCycle == curCycle);
    /* NOTE: Validation with weave mems shows that not advancing internal cycle
     * counters in e.g., the ROB does not change much; consider full-blown
     * rebases though if weave models fail to validate for some app.
     */
}

// Phase 1: stall the core for a fixed number of cycles while the
// accelerator executes.  Delegates to advance() which is defined above.
void OOOCore::stallForAccel(uint64_t cycles) {
    if (cycles > 0) advance(curCycle + cycles);
}

// Pin interface code

void OOOCore::LoadFunc(THREADID tid, ADDRINT addr, ADDRINT pc) {static_cast<OOOCore*>(cores[tid])->load(addr, pc);}
void OOOCore::StoreFunc(THREADID tid, ADDRINT addr, ADDRINT pc) {static_cast<OOOCore*>(cores[tid])->store(addr, pc);}

void OOOCore::PredLoadFunc(THREADID tid, ADDRINT addr, ADDRINT pc, BOOL pred) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    if (pred) core->load(addr, pc);
    else core->predFalseLoad();
}

void OOOCore::PredStoreFunc(THREADID tid, ADDRINT addr, ADDRINT pc, BOOL pred) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    if (pred) core->store(addr, pc);
    else core->predFalseStore();
}

void OOOCore::BblFunc(THREADID tid, ADDRINT bblAddr, BblInfo* bblInfo) {
    OOOCore* core = static_cast<OOOCore*>(cores[tid]);
    core->bbl(bblAddr, bblInfo);

    while (core->curCycle > core->phaseEndCycle) {
        core->phaseEndCycle += zinfo->phaseLength;

        uint32_t cid = getCid(tid);
        // NOTE: TakeBarrier may take ownership of the core, and so it will be used by some other thread. If TakeBarrier context-switches us,
        // the *only* safe option is to return inmmediately after we detect this, or we can race and corrupt core state. However, the information
        // here is insufficient to do that, so we could wind up double-counting phases.
        uint32_t newCid = TakeBarrier(tid, cid);
        // NOTE: Upon further observation, we cannot race if newCid == cid, so this code should be enough.
        // It may happen that we had an intervening context-switch and we are now back to the same core.
        // This is fine, since the loop looks at core values directly and there are no locals involved,
        // so we should just advance as needed and move on.
        if (newCid != cid) break;  /*context-switch, we do not own this context anymore*/
    }
}

void OOOCore::BranchFunc(THREADID tid, ADDRINT pc, BOOL taken, ADDRINT takenNpc, ADDRINT notTakenNpc) {
    static_cast<OOOCore*>(cores[tid])->branch(pc, taken, takenNpc, notTakenNpc);
}

