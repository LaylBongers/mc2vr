// mc2vr IPC protocol v1 (S4-1) — the carrier (win32, in-game) <-> host
// (win64, OpenXR) shared-memory contract. See docs/stereo_design.md §S4.
//
// One fixed-size section created by the HOST (server) via CreateFileMappingA
// and opened by the carrier/client with OpenFileMappingA. Both ends map it
// read/write. Everything here must be bit-identical across i386 and x86_64:
// fixed-width fields only, natural alignment, no pointers. All accessors are
// single-writer/single-reader lock-free (seqlock + SPSC rings) so neither
// side ever blocks the other (hard rule: the host never blocks the game).
//
// Section layout:
//   Mc2IpcBlock
//     header            magic/version/size/pids (host writes, carrier CAS-registers)
//     Mc2IpcState       latest-wins pose/state, seqlock-protected (host = writer)
//     Mc2IpcRing events   host -> carrier (session state, exit, recenter, ...)
//     Mc2IpcRing commands carrier -> host (shutdown, config, frame-ready, ...)
//
// Ordering: x86/x64 are TSO, so a store-release of the counter after the
// payload is a compiler barrier away; we use __sync_synchronize() before
// each counter store for that.

#ifndef MC2VR_IPC_H
#define MC2VR_IPC_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define MC2VR_IPC_MAGIC   0x4d325652u  // "M2VR"
#define MC2VR_IPC_VERSION 1u

// Carrier may override the section name via env MC2VR_IPC_NAME (selftest uses
// per-run names so a live session is never disturbed); default matches the
// host's. Names are passed to Create/OpenFileMappingA with the "Local\\"
// prefix applied by the callers.
#define MC2VR_IPC_DEFAULT_NAME "mc2vr_ipc_v1"

// ---- State block (seqlock; host is the only writer) -----------------------

typedef struct Mc2IpcVec3 { float x, y, z; } Mc2IpcVec3;
typedef struct Mc2IpcQuat { float x, y, z, w; } Mc2IpcQuat;
// OpenXR XrFovF half-ANGLES in radians (angleLeft/angleDown negative) — NOT
// tangents; consumers take tan() themselves.
typedef struct Mc2IpcFov { float left, right, up, down; } Mc2IpcFov;

typedef struct Mc2IpcEyePose {
    Mc2IpcVec3 pos;
    Mc2IpcQuat rot;
    Mc2IpcFov  fov;
} Mc2IpcEyePose;

// MC2VR_IPC_STF_* flags.
#define MC2VR_IPC_STF_TRACKED 0x1u

// XrSessionState values (mirrored so the header needs no OpenXR includes).
#define MC2VR_XR_SESSION_IDLE         1u
#define MC2VR_XR_SESSION_READY        2u
#define MC2VR_XR_SESSION_SYNCHRONIZED 3u
#define MC2VR_XR_SESSION_VISIBLE      4u
#define MC2VR_XR_SESSION_FOCUSED      5u
#define MC2VR_XR_SESSION_STOPPING     6u
#define MC2VR_XR_SESSION_LOSS_PENDING 7u
#define MC2VR_XR_SESSION_EXITING      8u

typedef struct Mc2IpcState {
    volatile uint32_t seq;        // even = stable, odd = write in progress
    uint32_t flags;               // MC2VR_IPC_STF_*
    int64_t  displayTime;         // XrTime ns (mock: QPC-derived)
    uint32_t sessionState;        // MC2VR_XR_SESSION_*
    uint32_t recenterCount;      // increments on every reference-space change
    uint32_t hostFrame;           // host publish counter; poseId = hostFrame+1 (S4-4:
                                  // the carrier echoes it in FRAME_READY.e so the host
                                  // can submit the layer with the pose that was rendered)
    float    ipd;                 // meters
    Mc2IpcEyePose eye[2];         // [0]=LEFT [1]=RIGHT
} Mc2IpcState;

// ---- Rings (SPSC; fixed-size messages) -------------------------------------

// Host -> carrier events.
#define MC2VR_MSG_SESSION_STATE 1u  // a=state b=recenterCount
#define MC2VR_MSG_EXIT         2u  // runtime wants the app to quit (S4-5)
#define MC2VR_MSG_RECENTER     3u  // user recentered (S4-5)

// Carrier -> host commands.
#define MC2VR_CMD_SHUTDOWN     1u  // clean host shutdown (host exits 0)
#define MC2VR_CMD_CONFIG       2u  // a=width b=height c=format d=reserved (S4-2)
#define MC2VR_CMD_FRAME_READY  3u  // x=frameId y=sharedHandle a=slot b=eye
                                   // c=width d=height (S4-2)
                                   // e=poseId (S4-4): the Mc2IpcState.hostFrame+1
                                   // the carrier rendered this frame with; 0 =
                                   // no HMD pose (static-pan render)

// One message fits every current and planned (S4-2 FrameReady) payload.
typedef struct Mc2IpcMsg {
    uint32_t type;
    uint32_t a, b, c, d;   // 32-bit payload words
    uint32_t e;            // fifth word (fills the former alignment pad; size unchanged)
    uint64_t x, y;         // 64-bit payload words (frameId, handles)
} Mc2IpcMsg;

#define MC2VR_MSG_WORDS 6u  // payload word count (for CRC-free size checks)

// Capacity: commands are rare (shutdown/config), events are rare too
// (state changes); generous enough to never drop in practice.
#define MC2VR_RING_EVENTS_CAP    128u
#define MC2VR_RING_COMMANDS_CAP  64u

typedef struct Mc2IpcRing {
    volatile uint32_t head;  // producer advances (counting, wraps at 2^32)
    volatile uint32_t tail;  // consumer advances (counting)
    uint32_t cap;
    uint32_t elemSize;
    Mc2IpcMsg buf[MC2VR_RING_EVENTS_CAP];
} Mc2IpcRing;

// ---- Section ---------------------------------------------------------------

typedef struct Mc2IpcBlock {
    // --- offset 0: header (written once by host; carrierPid via CAS) ---
    uint32_t magic;                 // MC2VR_IPC_MAGIC
    uint32_t version;              // MC2VR_IPC_VERSION
    uint32_t blockSize;           // sizeof(Mc2IpcBlock)
    uint32_t hostPid;
    volatile uint32_t carrierPid;  // 0 until the carrier registers (CAS)
    volatile uint32_t hostExiting; // host sets 1 right before it exits
    uint32_t reserved[2];
    // --- offset 32 ---
    Mc2IpcState  state;
    Mc2IpcRing   events;     // host -> carrier
    Mc2IpcRing   commands;  // carrier -> host
} Mc2IpcBlock;

// ---- Inline helpers (work in C and C++, MinGW i386 + x86_64) --------------
// Both sides include <windows.h> before this header.

// Seqlock write (host only). `src` is the host's scratch state; its `seq`
// word is ignored. The payload copy skips the seq word so a reader can
// never observe an even seq with torn data.
static inline void mc2_ipc_state_write(Mc2IpcBlock *blk, const Mc2IpcState *src) {
    Mc2IpcState *st = &blk->state;
    const uint32_t s = st->seq;
    st->seq = s + 1;  // odd => write section open
    __sync_synchronize();
    memcpy(&st->flags, &src->flags,
           sizeof(Mc2IpcState) - offsetof(Mc2IpcState, flags));
    __sync_synchronize();
    st->seq = s + 2;  // even => stable again
}

// Seqlock read with bounded retries. Returns 0 on success, -1 if the writer
// never settled (treat payload as stale/absent, never block).
static inline int mc2_ipc_state_read(const Mc2IpcBlock *blk, Mc2IpcState *out) {
    const volatile Mc2IpcState *st =
        (const volatile Mc2IpcState *)&blk->state;
    for (int i = 0; i < 32; ++i) {
        uint32_t s1 = st->seq;
        if (s1 & 1u) continue;  // mid-write
        __sync_synchronize();
        memcpy(&out->flags, (const void *)&st->flags,
               sizeof(Mc2IpcState) - offsetof(Mc2IpcState, flags));
        __sync_synchronize();
        if (st->seq == s1) return 0;  // consistent snapshot
    }
    return -1;
}

// SPSC push. Returns 0 on success, -1 if full (message dropped — caller logs).
static inline int mc2_ring_push(Mc2IpcRing *r, const Mc2IpcMsg *m) {
    uint32_t head = r->head, tail = r->tail;
    if (head - tail >= r->cap) return -1;
    Mc2IpcMsg *slot = &r->buf[head % r->cap];
    *slot = *m;
    __sync_synchronize();
    r->head = head + 1;
    return 0;
}

// SPSC pop. Returns 0 and fills *out, or -1 when empty.
static inline int mc2_ring_pop(Mc2IpcRing *r, Mc2IpcMsg *out) {
    uint32_t head = r->head, tail = r->tail;
    if (head == tail) return -1;
    __sync_synchronize();
    *out = r->buf[tail % r->cap];
    r->tail = tail + 1;
    return 0;
}

static inline int mc2_ring_pending(const Mc2IpcRing *r) {
    return (int)(r->head - r->tail);
}

// Carrier registers once; whoever wins the CAS from 0 is "the carrier".
// (__sync builtin rather than Interlocked*: the header must compile as C and
// C++ on both MinGW targets, and MinGW maps Interlocked* to macros.)
static inline int mc2_ipc_register_carrier(Mc2IpcBlock *blk, uint32_t pid) {
    return __sync_val_compare_and_swap(&blk->carrierPid, 0, pid) == 0;
}

#endif  // MC2VR_IPC_H
