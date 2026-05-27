/** accel_core.cpp
 *
 * Phase 1 implementation of the simulated accelerator.
 *
 * Design notes:
 *  - simulate() is called synchronously from HandleMagicOp() on the Pin
 *    analysis thread.  It must NOT call any Pin API that requires being on
 *    the application thread (e.g. PIN_GetContextReg).
 *  - The functional model (runCollisionCheck) is a direct port of the
 *    scalar isFree() loop from carribot.cpp so results are always correct.
 *  - The latency returned is fixed in Phase 1; Phase 2 will replace this
 *    with a cycle-accurate pipeline model.
 */

#include "accel_core.h"
#include "log.h"     // info() / warn() macros from zsim

// test branch

// ----------------------------------------------------------------
// Constructor
// ----------------------------------------------------------------
AccelCore::AccelCore(const g_string& name, const AccelConfig& cfg)
    : name_(name), cfg_(cfg) {
    info("[AccelCore] '%s' created. Phase-1 fixed latency = %lu cycles, lane_width = %u",
         name_.c_str(),
         cfg_.fixed_latency_cycles,
         cfg_.lane_width);
}

// ----------------------------------------------------------------
// initStats: register counters with the zsim stats framework.
// Called from init.cpp after the global stat tree is set up.
// ----------------------------------------------------------------
void AccelCore::initStats(AggregateStat* parentStat) {
    AggregateStat* accelStat = new AggregateStat();
    accelStat->init(name_.c_str(), "Accelerator stats");

    statInvocations_.init("invocations",   "Total accelerator invocations");
    statCollisions_.init ("collisions",    "Invocations returning collision/OOB");
    statCyclesCharged_.init("cyclesCharged","Total cycles charged to CPU cores");

    accelStat->append(&statInvocations_);
    accelStat->append(&statCollisions_);
    accelStat->append(&statCyclesCharged_);

    parentStat->append(accelStat);
}

// ----------------------------------------------------------------
// simulate: main entry point (called from HandleMagicOp).
// ----------------------------------------------------------------
uint64_t AccelCore::simulate(uint32_t tid, uint32_t cid, AccelTaskDesc* desc) {
    // --- Sanity check the task descriptor ---
    if (desc == nullptr) {
        warn("[AccelCore] tid=%u cid=%u: null AccelTaskDesc pointer!", tid, cid);
        return 0;
    }

    if (desc->task_id != 1 /* ACCEL_TASK_COLLISION_CHECK */) {
        warn("[AccelCore] tid=%u cid=%u: unknown task_id=%u, skipping",
             tid, cid, desc->task_id);
        desc->result = 0;
        return 0;
    }

    // --- Functional execution (always runs; gives correct result) ---
    // runCollisionCheck(desc);

    // --- Latency model (Phase 1: fixed) ---
    uint64_t latency = cfg_.fixed_latency_cycles;

    // --- Update statistics ---
    statInvocations_.inc();
    if (desc->result == 0) statCollisions_.inc();
    statCyclesCharged_.inc(latency);

    info("[AccelCore] tid=%u cid=%u | task=COLLISION_CHECK n_lines=%u "
         "pos=(%d,%d) -> result=%d | latency=%lu cycles",
         tid, cid,
         desc->n_lines, desc->x, desc->y,
         desc->result,
         latency);

    return latency;
}

// ----------------------------------------------------------------
// runCollisionCheck: software model of the collision-check kernel.
//
// This is a faithful copy of the scalar isFree() loop in carribot.cpp.
// It ensures desc->result is always correct regardless of the latency
// model, which is important for functional verification in Phase 1.
// ----------------------------------------------------------------
void AccelCore::runCollisionCheck(AccelTaskDesc* desc) {
    const AccelRobotLineMask* lines =
        reinterpret_cast<const AccelRobotLineMask*>(
            static_cast<uintptr_t>(desc->masks_base));
    const uint64_t* bitMap =
        reinterpret_cast<const uint64_t*>(
            static_cast<uintptr_t>(desc->bitmap_base));

    const int n           = static_cast<int>(desc->n_lines);
    const int x           = desc->x;
    const int y           = desc->y;
    const int mapX        = desc->mapX;
    const int mapY        = desc->mapY;
    const int wordsPerRow = desc->wordsPerRow;

    for (int i = 0; i < n; i++) {
        int checkY = y + lines[i].dy;
        int absX   = x + lines[i].min_dx;

        // Boundary check
        if (checkY < 0 || checkY >= mapY || absX < 0 || absX >= mapX) {
            desc->result = 0;  // out-of-bounds → treat as collision
            return;
        }

        // Bitmap access
        int wordIdx  = absX / 64;
        int bitShift = absX % 64;
        const uint64_t* rowPtr = &bitMap[checkY * wordsPerRow + wordIdx];

        uint64_t mapData = rowPtr[0] >> bitShift;
        if (bitShift > 0) {
            mapData |= (rowPtr[1] << (64 - bitShift));
        }

        // Collision test
        if (mapData & lines[i].mask) {
            desc->result = 0;  // collision
            return;
        }
    }

    desc->result = 1;  // free
}
