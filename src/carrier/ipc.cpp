#include "ipc.hpp"

#include <windows.h>

#include <cstdio>
#include <cstring>

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

// Low-rate monitor: logs session-state transitions + events from the host,
// watches the host process, and logs one first-pose evidence line. Deliberately
// NOT on the render thread — the render-thread consumer (S4-4) reads the
// seqlock directly.
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

        // Drain the event ring (events are rare; log each type once per run).
        Mc2IpcMsg ev;
        while (mc2_ring_pop(&g.blk->events, &ev) == 0) {
            switch (ev.type) {
            case MC2VR_MSG_SESSION_STATE:
                // The seqlock state above is the same info, fresher.
                break;
            case MC2VR_MSG_EXIT:
                MC2VR_LOG("ipc: host reports runtime exit request "
                          "(handled in S4-5)");
                break;
            case MC2VR_MSG_RECENTER:
                MC2VR_LOG("ipc: recenter (count %u)", ev.a);
                break;
            default:
                MC2VR_LOG("ipc: unknown event %u ignored", ev.type);
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

}  // namespace ipc
}  // namespace mc2vr
