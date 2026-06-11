/** vpu.cpp
 *
 * v1 implementation of the simulated Vector Processing Unit.  See vpu.h
 * for the high-level design.  Highlights:
 *
 *   - Vpu::execute() is called synchronously from HandleMagicOp on the
 *     Pin analysis thread.  It must NOT call any Pin API that requires
 *     being on the application thread.
 *   - The functional model is run on the simulator side and writes the
 *     result back into the descriptor (which lives in app memory and is
 *     directly addressable since we share the address space under Pin).
 *   - Memory traffic is sent through the host core's L1D so the existing
 *     coherence / contention infrastructure handles it automatically.
 */

#include "vpu.h"

#include "filter_cache.h"   // FilterCache::load / store
#include "log.h"            // info() / warn()

// ----------------------------------------------------------------
// Constructor
// ----------------------------------------------------------------
Vpu::Vpu(const g_string& name, const VpuConfig& cfg)
    : name_(name), cfg_(cfg) {
    info("[VPU] '%s' created. dispatch=%u exec=%u writeback=%u lane_width=%u",
         name_.c_str(),
         cfg_.dispatch_lat, cfg_.exec_lat, cfg_.writeback_lat, cfg_.lane_width);
}

// ----------------------------------------------------------------
// initStats: attach counters under 'parentStat' (typically rootStat).
// Must run before PostInitStats() makes the tree immutable.
// ----------------------------------------------------------------
void Vpu::initStats(AggregateStat* parentStat) {
    AggregateStat* vpuStat = new AggregateStat();
    vpuStat->init(name_.c_str(), "VPU co-processor stats");

    statInvocations_.init       ("invocations",       "Total VPU launches");
    statCyclesCharged_.init     ("cyclesCharged",     "Total cycles charged to host cores");
    statL1DAccesses_.init       ("l1dAccesses",       "L1D requests issued by the VPU");
    statElementsProcessed_.init ("elementsProcessed", "VEC_ADD: total elements computed");
    statUnknownOpcodes_.init    ("unknownOpcodes",    "Launches with unrecognised opcode");

    statCollisionInvocations_.init("collisionInvocations",
                                  "COLLISION_CHECK: total launches");
    statCollisionFree_.init       ("collisionFree",
                                  "COLLISION_CHECK: returned free (1)");
    statCollisionHit_.init        ("collisionHit",
                                  "COLLISION_CHECK: returned collision/OOB (0)");
    statLineLoads_.init           ("lineLoads",
                                  "COLLISION_CHECK: L1D loads of RobotLineMask entries");
    statBitmapLoads_.init         ("bitmapLoads",
                                  "COLLISION_CHECK: L1D loads of bitmap words");

    vpuStat->append(&statInvocations_);
    vpuStat->append(&statCyclesCharged_);
    vpuStat->append(&statL1DAccesses_);
    vpuStat->append(&statElementsProcessed_);
    vpuStat->append(&statUnknownOpcodes_);
    vpuStat->append(&statCollisionInvocations_);
    vpuStat->append(&statCollisionFree_);
    vpuStat->append(&statCollisionHit_);
    vpuStat->append(&statLineLoads_);
    vpuStat->append(&statBitmapLoads_);

    parentStat->append(vpuStat);
}

// ----------------------------------------------------------------
// execute: synchronous entry point.
//
// All host-side resources (L1D handle + starting cycle) are supplied
// by HandleMagicOp; this routine never touches zinfo->cores so it stays
// decoupled from any specific Core implementation.
// ----------------------------------------------------------------
uint64_t Vpu::execute(uint32_t tid, uint32_t cid, VpuTaskDesc* desc,
                      FilterCache* l1d, uint64_t hostStartCycle) {
    statInvocations_.inc();

    if (desc == nullptr) {
        warn("[VPU] tid=%u cid=%u: null descriptor pointer", tid, cid);
        return 0;
    }

    info("[VPU] enter: tid=%u cid=%u opcode=%u desc=%p l1d=%p startCycle=%lu",
         tid, cid, desc->opcode, (void*)desc, (void*)l1d, hostStartCycle);

    uint64_t curCycle = hostStartCycle + cfg_.dispatch_lat;

    switch (desc->opcode) {
        case VPU_OP_NOP:
            // Pure dispatch + writeback overhead.
            break;
        case VPU_OP_VEC_ADD:
            curCycle = doVecAdd(cid, l1d, curCycle, desc);
            break;
        case VPU_OP_COLLISION_CHECK:
            curCycle = doCollisionCheck(cid, l1d, curCycle, desc);
            break;
        default: {
            warn("[VPU] tid=%u cid=%u: unknown opcode=%u, returning -1",
                 tid, cid, desc->opcode);
            statUnknownOpcodes_.inc();
            desc->result = -1;
            const uint64_t lat = cfg_.dispatch_lat;
            statCyclesCharged_.inc(lat);
            return lat;
        }
    }

    curCycle += cfg_.writeback_lat;
    const uint64_t latency = curCycle - hostStartCycle;
    statCyclesCharged_.inc(latency);

    info("[VPU] exit:  tid=%u cid=%u opcode=%u result=%ld latency=%lu cycles",
         tid, cid, desc->opcode, (long)desc->result, latency);
    return latency;
}

// ----------------------------------------------------------------
// VPU_OP_VEC_ADD handler.
//
// Descriptor layout (set by the application):
//   arg[0] = a_ptr   (uint64_t*)
//   arg[1] = b_ptr   (uint64_t*)
//   arg[2] = c_ptr   (uint64_t*)   (output buffer, must be writable)
//   arg[3] = n       (number of int64 elements to process)
// On return:
//   desc->result = number of elements actually processed (or -1 on error)
//
// Functional behaviour: c[i] = a[i] + b[i] for i in [0, n).
// Memory model: each element issues 2 loads (a[i], b[i]) + 1 store (c[i])
//               through the host core's L1D, so the standard miss / hit
//               accounting kicks in.  When n > lane_width, batches are
//               processed serially (lane-level parallelism is approximated
//               only via the analytical latency below).
// ----------------------------------------------------------------
uint64_t Vpu::doVecAdd(uint32_t cid, FilterCache* l1d,
                       uint64_t curCycle, VpuTaskDesc* desc) {
    const int64_t* a = reinterpret_cast<const int64_t*>(
            static_cast<uintptr_t>(desc->arg[0]));
    const int64_t* b = reinterpret_cast<const int64_t*>(
            static_cast<uintptr_t>(desc->arg[1]));
    int64_t*       c = reinterpret_cast<int64_t*>(
            static_cast<uintptr_t>(desc->arg[2]));
    const uint64_t n = desc->arg[3];

    if (a == nullptr || b == nullptr || c == nullptr) {
        warn("[VPU][VEC_ADD] null pointer in descriptor (a=%p b=%p c=%p)",
             (void*)a, (void*)b, (void*)c);
        desc->result = -1;
        return curCycle;
    }

    // Synthetic "PC" tag for VPU-issued accesses, so the PC-access recorder
    // can distinguish them from regular core traffic.  Pick a value clearly
    // outside any real text-segment range.
    const Address vpuPc = 0xFFFFFFFFFFFF0000ULL | (uint64_t)desc->opcode;

    uint64_t reqCycle = curCycle;
    for (uint64_t i = 0; i < n; i++) {
        // Functional execution (always correct, independent of timing).
        c[i] = a[i] + b[i];

        if (l1d != nullptr) {
            // Issue the three memory ops through the host's L1D.  We chain
            // the response cycles so each access starts after the previous
            // one finishes; lane parallelism is folded in analytically
            // below via lane_width.
            uint64_t r1 = l1d->load (reinterpret_cast<Address>(&a[i]),
                                     reqCycle, vpuPc);
            uint64_t r2 = l1d->load (reinterpret_cast<Address>(&b[i]),
                                     r1, vpuPc);
            uint64_t r3 = l1d->store(reinterpret_cast<Address>(&c[i]),
                                     r2, vpuPc);
            reqCycle = r3;
            statL1DAccesses_.inc(3);
        }
    }

    // Analytical compute latency: ceil(n / lane_width) * exec_lat.
    const uint32_t lanes = (cfg_.lane_width > 0) ? cfg_.lane_width : 1;
    const uint64_t batches = (n + lanes - 1) / lanes;
    const uint64_t computeLat = batches * cfg_.exec_lat;

    statElementsProcessed_.inc(n);
    desc->result      = static_cast<int64_t>(n);
    desc->aux[0]      = batches;

    // Final cycle = max(memory completion, compute completion).
    const uint64_t computeDone = curCycle + computeLat;
    return (reqCycle > computeDone) ? reqCycle : computeDone;
}

// ----------------------------------------------------------------
// VPU_OP_COLLISION_CHECK handler.
//
// Descriptor layout (filled by the application):
//   arg[0] = bitmap_base    (uint64_t*)             host pointer to bitMap
//   arg[1] = lines_base     (VpuRobotLineMask*)     host pointer to lines
//   arg[2] = n_lines        (uint64_t)              number of robot rows
//   arg[3] = packed (x:lo32, y:hi32)                robot center, signed int32
//   arg[4] = packed (mapX:lo32, mapY:hi32)          map size, signed int32
//   arg[5] = wordsPerRow    (uint32_t in low bits)  bitmap row stride
// On return:
//   desc->result = 1 (free) | 0 (collision/OOB) | -1 (parameter error)
//   desc->aux[0] = number of lines actually processed (<= n_lines)
//   desc->aux[1] = reserved
//
// Functional behaviour: faithful port of carribot.cpp's scalar isFree().
//
// Memory model (serial, no MLP for v1):
//   For each line, in order, until OOB / collision / done:
//     1) load &lines[i]                  (16 B record)
//     2) AGU + boundary check            (functional)
//     3) load &bitMap[idx]               (1st bitmap word)
//     4) if bitShift>0: load &bitMap[idx+1]  (2nd bitmap word for shift)
//     5) compose mapData + mask test     (functional)
//   Each L1D access is chained through respCycle, so the next access
//   only starts after the previous one completes (serial gather).
// ----------------------------------------------------------------
uint64_t Vpu::doCollisionCheck(uint32_t cid, FilterCache* l1d,
                               uint64_t curCycle, VpuTaskDesc* desc) {
    statCollisionInvocations_.inc();

    // ---- Unpack descriptor ----
    const uint64_t* bitmap_base = reinterpret_cast<const uint64_t*>(
            static_cast<uintptr_t>(desc->arg[0]));
    const VpuRobotLineMask* lines = reinterpret_cast<const VpuRobotLineMask*>(
            static_cast<uintptr_t>(desc->arg[1]));
    const uint64_t n_lines = desc->arg[2];

    const uint64_t xy_packed   = desc->arg[3];
    const uint64_t map_packed  = desc->arg[4];
    const int32_t  x    = static_cast<int32_t>( xy_packed        & 0xFFFFFFFFULL);
    const int32_t  y    = static_cast<int32_t>((xy_packed  >> 32) & 0xFFFFFFFFULL);
    const int32_t  mapX = static_cast<int32_t>( map_packed        & 0xFFFFFFFFULL);
    const int32_t  mapY = static_cast<int32_t>((map_packed >> 32) & 0xFFFFFFFFULL);
    const uint32_t wordsPerRow = static_cast<uint32_t>(desc->arg[5] & 0xFFFFFFFFULL);

    if (bitmap_base == nullptr || lines == nullptr) {
        warn("[VPU][COLLISION] null pointer in descriptor (bitmap=%p lines=%p)",
             (void*)bitmap_base, (void*)lines);
        desc->result = -1;
        desc->aux[0] = 0;
        return curCycle;
    }

    // Synthetic "PC" tag for VPU-issued accesses (distinct from VEC_ADD).
    const Address vpuPc = 0xFFFFFFFFFFFF0000ULL | (uint64_t)desc->opcode;

    int     result          = 1;   // assume free
    uint64_t lines_processed = 0;
    uint64_t reqCycle       = curCycle;

    for (uint64_t i = 0; i < n_lines; i++) {
        lines_processed++;

        // ---- Stage 1: LineFetch (cache load of lines[i]) ----
        if (l1d != nullptr) {
            reqCycle = l1d->load(
                    reinterpret_cast<Address>(&lines[i]), reqCycle, vpuPc);
            statLineLoads_.inc();
            statL1DAccesses_.inc();
        }
        const int32_t  dy     = lines[i].dy;
        const int32_t  min_dx = lines[i].min_dx;
        const uint64_t mask   = lines[i].mask;

        // ---- Stage 2: AGU + Stage 3: boundary check ----
        const int32_t checkY = y + dy;
        const int32_t absX   = x + min_dx;
        if (checkY < 0 || checkY >= mapY || absX < 0 || absX >= mapX) {
            result = 0;   // out-of-bounds counts as collision
            break;
        }

        const int32_t  wordIdx  = absX >> 6;          // absX / 64
        const int32_t  bitShift = absX & 63;          // absX % 64
        const uint64_t* rowPtr  =
                &bitmap_base[checkY * wordsPerRow + wordIdx];

        // ---- Stage 4: BitmapGet0 ----
        if (l1d != nullptr) {
            reqCycle = l1d->load(
                    reinterpret_cast<Address>(&rowPtr[0]), reqCycle, vpuPc);
            statBitmapLoads_.inc();
            statL1DAccesses_.inc();
        }
        uint64_t mapData = rowPtr[0] >> bitShift;

        // ---- Stage 5: BitmapGet1 (only when shift > 0) ----
        if (bitShift > 0) {
            if (l1d != nullptr) {
                reqCycle = l1d->load(
                        reinterpret_cast<Address>(&rowPtr[1]), reqCycle, vpuPc);
                statBitmapLoads_.inc();
                statL1DAccesses_.inc();
            }
            mapData |= (rowPtr[1] << (64 - bitShift));
        }

        // ---- Stage 6: Compose + collision test ----
        if (mapData & mask) {
            result = 0;
            break;
        }
    }

    // Compute latency: one "exec_lat" tick per processed line, modelling
    // the per-line AGU + compose+test pipeline.  Memory and compute run
    // in parallel; the final cycle is max(memory_done, compute_done).
    const uint64_t computeLat  = lines_processed * cfg_.exec_lat;
    const uint64_t computeDone = curCycle + computeLat;

    desc->result = result;
    desc->aux[0] = lines_processed;
    desc->aux[1] = 0;
    if (result == 1) statCollisionFree_.inc();
    else             statCollisionHit_.inc();

    return (reqCycle > computeDone) ? reqCycle : computeDone;
}
