#include "ipc.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

#include "game_addresses.h"
#include "log.hpp"

namespace mc2vr {
namespace ipc {

namespace {

struct Client {
    HANDLE section = nullptr;
    Mc2IpcBlock *blk = nullptr;
    HANDLE hostProc = nullptr;   // death watch
    volatile bool alive = false; // host section observed live
    bool stopSent = false;
};
Client g;

const char *session_state_name(uint32_t s)
{
    switch (s) {
    case MC2VR_XR_SESSION_IDLE: return "IDLE";
    case MC2VR_XR_SESSION_READY: return "READY";
    case MC2VR_XR_SESSION_SYNCHRONIZED: return "SYNCHRONIZED";
    case MC2VR_XR_SESSION_VISIBLE: return "VISIBLE";
    case MC2VR_XR_SESSION_FOCUSED: return "FOCUSED";
    case MC2VR_XR_SESSION_STOPPING: return "STOPPING";
    case MC2VR_XR_SESSION_LOSS_PENDING: return "LOSS_PENDING";
    case MC2VR_XR_SESSION_EXITING: return "EXITING";
    default: return "UNKNOWN";
    }
}

// Low-rate monitor: logs session-state transitions (via the seqlock state,
// which is what covers early boot before the slot-5 claim) + first-pose
// evidence, and watches the host process. Deliberately NOT on the render
// thread — the render-thread consumer (S4-4) reads the seqlock directly.
// S4-5: the event-ring drain moved to ipc::drain_events() (slot-5
// PostUpdateHook, main thread) — the ring is SPSC, so this thread must NEVER
// pop events, or the two consumers would tear the ring.
DWORD WINAPI monitor_thread(void *)
{
    uint32_t lastState = ~0u;
    bool loggedPose = false;
    for (;;) {
        Sleep(250);
        if (!g.alive || g.blk == nullptr) return 0;

        Mc2IpcState st;
        if (mc2_ipc_state_read(g.blk, &st) == 0) {
            if (st.sessionState != lastState) {
                lastState = st.sessionState;
                MC2VR_LOG("ipc: session state -> %s (host frame %u, ipd %.4f)",
                          session_state_name(st.sessionState), st.hostFrame, st.ipd);
            }
            if (!loggedPose && (st.flags & MC2VR_IPC_STF_TRACKED)) {
                loggedPose = true;
                MC2VR_LOG("ipc: first tracked pose L=(%.3f %.3f %.3f) "
                          "fovL=(%.3f %.3f %.3f %.3f)",
                          st.eye[0].pos.x, st.eye[0].pos.y, st.eye[0].pos.z,
                          st.eye[0].fov.left, st.eye[0].fov.right,
                          st.eye[0].fov.up, st.eye[0].fov.down);
            }
        }

        // Host death watch: explicit flag or process handle.
        if (g.blk->hostExiting) {
            MC2VR_LOG("ipc: host exited — continuing standalone (monitor off)");
            g.alive = false;
            return 0;
        }
        if (g.hostProc != nullptr &&
            WaitForSingleObject(g.hostProc, 0) == WAIT_OBJECT_0) {
            MC2VR_LOG("ipc: host process died — continuing standalone "
                      "(monitor off)");
            g.alive = false;
            return 0;
        }
    }
}

}  // namespace

void connect()
{
    char name[128] = MC2VR_IPC_DEFAULT_NAME;
    char env[128];
    const DWORD n = GetEnvironmentVariableA("MC2VR_IPC_NAME", env, sizeof env);
    if (n > 0 && n < sizeof env) {
        lstrcpynA(name, env, sizeof name);
    }

    char objName[160];
    snprintf(objName, sizeof objName, "Local\\%s", name);
    g.section = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, objName);
    if (g.section == nullptr) {
        // Expected whenever the host isn't running — the game continues
        // exactly as today. Log once and stay quiet afterwards.
        MC2VR_LOG("ipc: no host section '%s' — running standalone", name);
        return;
    }
    g.blk = (Mc2IpcBlock *)MapViewOfFile(g.section, FILE_MAP_ALL_ACCESS, 0, 0,
                                        sizeof(Mc2IpcBlock));
    if (g.blk == nullptr) {
        MC2VR_LOG("ipc: MapViewOfFile failed (%lu) — running standalone",
                  GetLastError());
        CloseHandle(g.section);
        g.section = nullptr;
        return;
    }
    if (g.blk->magic != MC2VR_IPC_MAGIC || g.blk->version != MC2VR_IPC_VERSION) {
        MC2VR_LOG("ipc: section magic/version mismatch (host is a different "
                  "build?) — running standalone");
        UnmapViewOfFile(g.blk);
        g.blk = nullptr;
        CloseHandle(g.section);
        g.section = nullptr;
        return;
    }

    mc2_ipc_register_carrier(g.blk, GetCurrentProcessId());
    g.hostProc = OpenProcess(SYNCHRONIZE, FALSE, g.blk->hostPid);
    g.alive = true;

    MC2VR_LOG("ipc: connected to host (pid %u, block %u bytes)",
              g.blk->hostPid, g.blk->blockSize);

    // Leaked by design like every carrier thread (see launcher_plan.md).
    HANDLE t = CreateThread(nullptr, 0, monitor_thread, nullptr, 0, nullptr);
    if (t != nullptr) CloseHandle(t);
}

bool connected()
{
    return g.alive && g.blk != nullptr;
}

bool read_state(Mc2IpcState *out)
{
    if (!connected()) return false;
    return mc2_ipc_state_read(g.blk, out) == 0;
}

bool pop_event(Mc2IpcMsg *out)
{
    if (!connected()) return false;
    return mc2_ring_pop(&g.blk->events, out) == 0;
}

// ---- S4-5 session events (drained at the slot-5 PostUpdateHook) -------------

void drain_events()
{
    if (!connected()) return;

    static uint32_t logged_state = 0; // last state this drain logged (0 = none)
    static bool exit_sent = false;   // one-shot WM_CLOSE guard

    Mc2IpcMsg ev;
    while (mc2_ring_pop(&g.blk->events, &ev) == 0) {
        switch (ev.type) {
        case MC2VR_MSG_SESSION_STATE:
            // Log transitions once (events repeat the same state if the host
            // re-pushes; the seqlock monitor may also log early-boot
            // transitions — a rare duplicate line is fine).
            if (ev.a != logged_state) {
                logged_state = ev.a;
                MC2VR_LOG("session: state -> %s (recenter=%u)",
                          session_state_name(ev.a), ev.b);
            }
            break;
        case MC2VR_MSG_RECENTER:
            // Nothing to apply: the hmd camera consumes live HMD poses, so a
            // reference-space change propagates at the next pass-1 pose
            // sample by construction. Logged so recenter jumps are
            // attributable in hindsight.
            MC2VR_LOG("session: recenter #%u — camera follows the new "
                      "reference space at the next pose sample", ev.a);
            break;
        case MC2VR_MSG_EXIT:
            // Runtime wants the app to quit (instance loss / runtime-initiated
            // session end). Clean shutdown = let the engine quit itself:
            // WM_CLOSE to the game's root window. The engine's pump/WndProc
            // quit path is VM-protected (PostQuitMessage/GetMessageA are only
            // SecuROM-region references), so we can't invoke it directly —
            // the message is the standard, engine-expected signal. One-shot:
            // a second event must not stack another WM_CLOSE.
            if (!exit_sent) {
                exit_sent = true;
                HWND hwnd = (HWND)*(volatile uintptr_t *)MC2_G_RENDER_HWND;
                if (hwnd != nullptr) {
                    HWND root = GetAncestor(hwnd, GA_ROOT);
                    if (root != nullptr) hwnd = root;
                }
                if (hwnd != nullptr && PostMessageW(hwnd, WM_CLOSE, 0, 0) != 0) {
                    MC2VR_LOG("session: runtime exit request (event payload %u) "
                              "— posted WM_CLOSE to the game window %p; "
                              "clean shutdown follows via the engine's pump",
                              ev.a, hwnd);
                } else {
                    MC2VR_LOG("session: runtime exit request (event payload %u) "
                              "— no postable game window yet (hwnd=%p), cannot "
                              "quit cleanly",
                              ev.a, hwnd);
                }
            }
            break;
        default:
            MC2VR_LOG("session: unknown event %u ignored", ev.type);
        }
    }
}

void send_shutdown()
{
    if (!connected() || g.stopSent) return;
    g.stopSent = true;
    Mc2IpcMsg m;
    ZeroMemory(&m, sizeof m);
    m.type = MC2VR_CMD_SHUTDOWN;
    if (mc2_ring_push(&g.blk->commands, &m) == 0) {
        MC2VR_LOG("ipc: Shutdown queued");
    } else {
        MC2VR_LOG("ipc: command ring full — Shutdown not delivered");
        g.stopSent = false;
    }
}

bool push_command(Mc2IpcMsg *m)
{
    if (!connected()) return false;
    return mc2_ring_push(&g.blk->commands, m) == 0;
}

bool send_config(uint32_t width, uint32_t height, uint32_t format)
{
    if (!connected()) return false;
    Mc2IpcMsg m;
    ZeroMemory(&m, sizeof m);
    m.type = MC2VR_CMD_CONFIG;
    m.a = width;
    m.b = height;
    m.c = format;
    m.d = 0;
    if (!push_command(&m)) return false;
    MC2VR_LOG("ipc: Config queued (%ux%u fmt %u)", width, height, format);
    return true;
}

bool send_frame_ready(uint64_t frameId, uint64_t handle, uint32_t slot,
                      uint32_t eye, uint32_t width, uint32_t height,
                      uint32_t poseId)
{
    if (!connected()) return false;
    Mc2IpcMsg m;
    ZeroMemory(&m, sizeof m);
    m.type = MC2VR_CMD_FRAME_READY;
    m.x = frameId;
    m.y = handle;
    m.a = slot;
    m.b = eye;
    m.c = width;
    m.d = height;
    m.e = poseId;
    return push_command(&m);
}

}  // namespace ipc
}  // namespace mc2vr
