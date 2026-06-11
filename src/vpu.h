/** vpu.h
 *
 * Simulated Vector Processing Unit (VPU) co-processor for zsim.
 *
 * Design summary (v1):
 *   - Top-level entity is independent from any Core.  It is owned by
 *     the global GlobSimInfo (zinfo->vpu) and is awakened by the
 *     application through a magic-op hook (see apps/include/zsim_hooks.h).
 *   - Synchronous semantics: HandleMagicOp calls Vpu::execute() on the
 *     analysis thread; execute() runs the functional model, models the
 *     latency, and returns the latency in cycles.  The host core then
 *     advances its cycle counter by that latency via Core::stallCycles().
 *   - Cache access: the VPU does not own a cache; it issues loads/stores
 *     through the host core's L1D (obtained via Core::getL1D()), so VPU
 *     traffic naturally shares the host's coherence domain and shows up
 *     as L1D contention in the existing cache hierarchy.
 *
 * Magic-op encoding (sentinel + 48-bit pointer, see HandleMagicOp):
 *   op[63:48] = ZSIM_VPU_MAGIC_SENTINEL  (0xCAFE)
 *   op[47:0]  = (uintptr_t)(VpuTaskDesc*)
 *
 * The opcode (VEC_ADD / ...) is carried inside the descriptor, so a
 * single magic-op encoding is sufficient for v1 (synchronous launch).
 *
 * Future extensions (not in v1):
 *   - Asynchronous launch + explicit wait / fence ops
 *   - Multi-lane pipeline modelling (replace fixed latency with stages)
 *   - VPU-private L1V cache + MESI participation
 *   - Custom ISA extension via the Pin decoder path
 */

#ifndef VPU_H_
#define VPU_H_

#include <stddef.h>            // offsetof
#include <stdint.h>
#include "galloc.h"            // GlobAlloc
#include "g_std/g_string.h"
#include "stats.h"             // Counter, AggregateStat

// Forward declarations to keep this header light.
class FilterCache;

// ----------------------------------------------------------------
// VpuTaskDesc: generic descriptor for one VPU invocation.
// MUST match the layout in apps/include/zsim_hooks.h on the app side.
// ----------------------------------------------------------------
struct VpuTaskDesc {
    uint32_t opcode;       // VPU_OP_* (see zsim_hooks.h)
    uint32_t flags;        // reserved for per-op flags
    uint64_t arg[6];       // per-op arguments (pointers / scalars)
    int64_t  result;       // primary return value (sim writes)
    uint64_t aux[2];       // auxiliary return values (e.g. counters)
};

// VPU opcodes shared with the application side.  Keep this list in sync
// with the VPU_OP_* macros in apps/include/zsim_hooks.h.
enum VpuOpcode : uint32_t {
    VPU_OP_NOP              = 0,   // does nothing, returns 0 latency
    VPU_OP_VEC_ADD          = 1,   // c[i] = a[i] + b[i] for i in [0, n)
    VPU_OP_COLLISION_CHECK  = 2,   // bitmap-based 2D robot collision check
    VPU_OP_LAST_                   // sentinel; keep last
};

// ----------------------------------------------------------------
// VpuRobotLineMask: simulator-side mirror of the application's
// RobotLineMask layout.  The application passes an array of these via
// desc->arg[1] for VPU_OP_COLLISION_CHECK.  The layout MUST match the
// app-side struct byte-for-byte; the static_asserts below pin it down.
// ----------------------------------------------------------------
struct VpuRobotLineMask {
    int32_t  dy;          // row offset relative to robot center y
    int32_t  min_dx;      // leftmost column offset relative to x
    uint64_t mask;        // shape mask of this row (1 = robot occupies)
};
static_assert(sizeof(VpuRobotLineMask) == 16,
              "VpuRobotLineMask must be 16 bytes (matches app RobotLineMask)");
static_assert(offsetof(VpuRobotLineMask, dy)     == 0, "layout mismatch");
static_assert(offsetof(VpuRobotLineMask, min_dx) == 4, "layout mismatch");
static_assert(offsetof(VpuRobotLineMask, mask)   == 8, "layout mismatch");

// ----------------------------------------------------------------
// VpuConfig: tunable parameters read from the zsim config file
// (vpu.* keys, see init.cpp).
// ----------------------------------------------------------------
struct VpuConfig {
    // Per-launch fixed overhead (cycles) charged once at dispatch.
    uint32_t dispatch_lat;
    // Per-element compute latency (cycles per scalar lane).
    uint32_t exec_lat;
    // Writeback / completion overhead (cycles) charged once at the end.
    uint32_t writeback_lat;
    // Number of parallel lanes (vector width, in elements per cycle).
    // v1 only uses this for the simple analytical latency formula.
    uint32_t lane_width;

    VpuConfig()
        : dispatch_lat(10),
          exec_lat(1),
          writeback_lat(5),
          lane_width(8) {}
};

// ----------------------------------------------------------------
// Vpu: top-level simulated vector processing unit.
// ----------------------------------------------------------------
class Vpu : public GlobAlloc {
  public:
    explicit Vpu(const g_string& name, const VpuConfig& cfg = VpuConfig());

    // Register stat counters under parentStat.  Must be called BEFORE
    // PostInitStats() so the root stat tree is still mutable.
    void initStats(AggregateStat* parentStat);

    // Main synchronous entry point, called from HandleMagicOp().
    //   tid             - Pin thread id of the calling CPU thread
    //   cid             - core id of the host CPU core that issued the magic op
    //   desc            - pointer into application memory (input + output fields)
    //   l1d             - host core's L1 data cache (may be nullptr)
    //   hostStartCycle  - host core's current cycle (used as t=0 for cache reqs)
    // Returns: total latency in cycles to charge to the host core.
    // Side effect: writes desc->result (and possibly desc->aux[]).
    //
    // NOTE: We intentionally do NOT look up zinfo->cores[cid] inside this
    // method.  HandleMagicOp is the only place that touches the cores
    // array; everything the VPU needs is passed in as a parameter so the
    // VPU is decoupled from any specific Core implementation.
    uint64_t execute(uint32_t tid, uint32_t cid, VpuTaskDesc* desc,
                     FilterCache* l1d, uint64_t hostStartCycle);

  private:
    g_string  name_;
    VpuConfig cfg_;

    // ---- Stats ----
    Counter   statInvocations_;       // total VPU launches
    Counter   statCyclesCharged_;     // total cycles charged to host cores
    Counter   statL1DAccesses_;       // total L1D requests issued by the VPU
    Counter   statElementsProcessed_; // VEC_ADD: total elements computed
    Counter   statUnknownOpcodes_;    // launches with unrecognised opcode

    // ---- Stats specific to VPU_OP_COLLISION_CHECK ----
    Counter   statCollisionInvocations_;  // # of collision-check launches
    Counter   statCollisionFree_;         // # that returned free (1)
    Counter   statCollisionHit_;          // # that returned collision/OOB (0)
    Counter   statLineLoads_;             // L1D loads of RobotLineMask entries
    Counter   statBitmapLoads_;           // L1D loads of bitmap words

    // ---- Per-opcode handlers ----
    // Each handler advances 'curCycle' through dispatch / exec / writeback,
    // performs the functional computation (writing desc->result), and
    // returns the new curCycle.  L1D is the host core's L1 data cache or
    // nullptr if the host core does not model an L1D (e.g. NullCore).
    uint64_t doVecAdd(uint32_t cid, FilterCache* l1d,
                      uint64_t curCycle, VpuTaskDesc* desc);
    uint64_t doCollisionCheck(uint32_t cid, FilterCache* l1d,
                              uint64_t curCycle, VpuTaskDesc* desc);
};

#endif  // VPU_H_
