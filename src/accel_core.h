/** accel_core.h
 *
 * Simulated accelerator (co-processor) for collision-check offloading.
 *
 * Phase 1: stub / fixed-latency model.
 *   - AccelCore::simulate() receives an AccelTaskDesc pointer that was
 *     passed from the application via the ZSIM_MAGIC_OP_ACCEL_INVOKE
 *     magic op.
 *   - It executes the task functionally (software model) so the result
 *     field in the descriptor is always correct.
 *   - It returns a modelled cycle latency that the CPU core uses to
 *     advance its cycle counter (stall-on-offload model).
 *
 * Phase 2 (future): replace fixed latency with a cycle-accurate
 *   pipeline model that walks through Dispatch / Fetch / Gather /
 *   BitOp / Writeback stages and queries the cache hierarchy.
 */

#ifndef ACCEL_CORE_H_
#define ACCEL_CORE_H_

#include <stdint.h>
#include "galloc.h"       // GlobAlloc base class (zsim shared-memory allocator)
#include "g_std/g_string.h"
#include "stats.h"        // Counter, AggregateStat

// ----------------------------------------------------------------
// AccelTaskDesc must match the layout declared in zsim_hooks.h.
// We redeclare it here (simulator side) so accel_core.cpp can be
// compiled without pulling in the full app-side header.
// ----------------------------------------------------------------
struct AccelTaskDesc {
    // --- Input fields ---
    uint32_t  task_id;
    uint32_t  n_lines;
    uint64_t  bitmap_base;   // host pointer to bitMap data (uint64_t*)
    uint64_t  masks_base;    // host pointer to robotMasks[theta] data
    int32_t   x;
    int32_t   y;
    int32_t   mapX;
    int32_t   mapY;
    int32_t   wordsPerRow;
    int32_t   _pad;
    // --- Output fields ---
    int32_t   result;        // 1 = free, 0 = collision / OOB
    int32_t   _pad2;
};

// Layout of one RobotLineMask entry (must match the app struct)
struct AccelRobotLineMask {
    int      dy;
    int      min_dx;
    uint64_t mask;
};

// ----------------------------------------------------------------
// AccelConfig: all tunable parameters for the accelerator model.
// Phase 1 uses only fixed_latency_cycles; later phases will use
// the individual stage latencies.
// ----------------------------------------------------------------
struct AccelConfig {
    // ---- Phase 1: fixed total latency (cycles) ----
    // The CPU core stalls for exactly this many cycles per invocation.
    // Set to 0 to make the accelerator appear instantaneous (useful for
    // functional-only verification runs).
    uint64_t fixed_latency_cycles;

    // ---- Phase 2+ placeholders (not used in Phase 1) ----
    uint32_t dispatch_lat;       // CPU -> Accel interface overhead
    uint32_t addr_compute_lat;   // address generation stage
    uint32_t bitop_lat;          // shift/AND stage per batch
    uint32_t writeback_lat;      // result writeback stage
    uint32_t lane_width;         // number of parallel processing lanes

    // Default constructor: conservative but non-zero latency
    AccelConfig()
        : fixed_latency_cycles(50),
          dispatch_lat(10),
          addr_compute_lat(2),
          bitop_lat(1),
          writeback_lat(5),
          lane_width(8) {}
};

// ----------------------------------------------------------------
// AccelCore: the top-level accelerator model class.
// ----------------------------------------------------------------
class AccelCore : public GlobAlloc {
  public:
    // Construct with a name (used in stats output) and optional config.
    explicit AccelCore(const g_string& name,
                       const AccelConfig& cfg = AccelConfig());

    // Initialise statistics counters and attach them to the parent stat node.
    void initStats(AggregateStat* parentStat);

    // Main entry point called from HandleMagicOp (zsim.cpp).
    //
    //   tid  - Pin thread id of the calling CPU thread
    //   cid  - core id of the CPU core that issued the magic op
    //   desc - pointer into application memory; both input and output fields
    //
    // Returns: modelled accelerator latency in cycles.
    // Side effect: fills desc->result with the correct collision answer.
    uint64_t simulate(uint32_t tid, uint32_t cid, AccelTaskDesc* desc);

  private:
    g_string   name_;
    AccelConfig cfg_;

    // ---- Statistics (Phase 1) ----
    Counter statInvocations_;    // total number of accelerator calls
    Counter statCollisions_;     // calls that returned result=0 (collision/OOB)
    Counter statCyclesCharged_;  // total cycles charged to CPU cores

    // ---- Functional implementation ----
    // Runs the software model of the collision check and writes desc->result.
    void runCollisionCheck(AccelTaskDesc* desc);
};

#endif  // ACCEL_CORE_H_
