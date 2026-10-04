#include "ipc.hpp"

#include <windows.h>

#include <cstdio>

#include "log.hpp"

namespace ipc {

namespace {

struct Server {
    HANDLE section = nullptr;
    Mc2IpcBlock* blk = nullptr;
    HANDLE carrierProc = nullptr;
    DWORD lastDeathCheck = 0;
    bool warnedCarrierDead = false;
};
Server g;

bool enabled() { return g.blk != nullptr; }

}  // namespace

bool server_init(const char* name) {
    char objName[128];
    snprintf(objName, sizeof objName, "Local\\%s", name);

    g.section = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                   0, sizeof(Mc2IpcBlock), objName);
    if (g.section == nullptr) {
        hostlog::write("ipc: CreateFileMappingA failed (%lu)", GetLastError());
        return false;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // Another host owns the section — two hosts would both write the
        // seqlock state. Refuse; the launcher proceeds without VR.
        hostlog::write("ipc: section '%s' already exists — another host is "
                       "running; exiting", objName);
        CloseHandle(g.section);
        g.section = nullptr;
        return false;
    }

    g.blk = (Mc2IpcBlock*)MapViewOfFile(g.section, FILE_MAP_ALL_ACCESS, 0, 0,
                                        sizeof(Mc2IpcBlock));
    if (g.blk == nullptr) {
        hostlog::write("ipc: MapViewOfFile failed (%lu)", GetLastError());
        CloseHandle(g.section);
        g.section = nullptr;
        return false;
    }

    Mc2IpcBlock* b = g.blk;
    ZeroMemory(b, sizeof *b);
    b->magic = MC2VR_IPC_MAGIC;
    b->version = MC2VR_IPC_VERSION;
    b->blockSize = sizeof(Mc2IpcBlock);
    b->hostPid = GetCurrentProcessId();
    b->state.seq = 0;
    b->state.sessionState = MC2VR_XR_SESSION_IDLE;
    b->events.cap = MC2VR_RING_EVENTS_CAP;
    b->events.elemSize = sizeof(Mc2IpcMsg);
    b->commands.cap = MC2VR_RING_COMMANDS_CAP;
    b->commands.elemSize = sizeof(Mc2IpcMsg);

    hostlog::write("ipc: section '%s' created (block %u bytes, host pid %lu)",
                   objName, b->blockSize, b->hostPid);
    return true;
}

void publish(const HmdFrame& f, float ipd, uint32_t sessionState,
             uint32_t recenterCount, uint32_t hostFrame) {
    if (!enabled()) return;
    Mc2IpcState st;
    ZeroMemory(&st, sizeof st);
    st.flags = f.tracked ? MC2VR_IPC_STF_TRACKED : 0;
    st.displayTime = f.displayTime;
    st.sessionState = sessionState;
    st.recenterCount = recenterCount;
    st.hostFrame = hostFrame;
    for (int e = 0; e < 2; ++e) {
        st.eye[e].pos = {f.eye[e].pos.x, f.eye[e].pos.y, f.eye[e].pos.z};
        st.eye[e].rot = {f.eye[e].rot.x, f.eye[e].rot.y, f.eye[e].rot.z,
                         f.eye[e].rot.w};
        st.eye[e].fov = {f.eye[e].fov.left, f.eye[e].fov.right, f.eye[e].fov.up,
                         f.eye[e].fov.down};
    }
    st.ipd = ipd;
    mc2_ipc_state_write(g.blk, &st);
}

void push_event(uint32_t type, uint32_t a, uint32_t b) {
    if (!enabled()) return;
    Mc2IpcMsg m;
    ZeroMemory(&m, sizeof m);
    m.type = type;
    m.a = a;
    m.b = b;
    if (mc2_ring_push(&g.blk->events, &m) != 0) {
        hostlog::write("ipc: event ring full — event %u dropped", type);
    }
}

bool pop_command(Mc2IpcMsg* out) {
    if (!enabled()) return false;
    return mc2_ring_pop(&g.blk->commands, out) == 0;
}

bool carrier_connected() { return enabled() && g.blk->carrierPid != 0; }

uint32_t carrier_pid() { return enabled() ? g.blk->carrierPid : 0; }

bool carrier_died(unsigned checkIntervalMs) {
    if (!enabled()) return false;
    const uint32_t pid = g.blk->carrierPid;
    if (pid == 0) return false;

    // Open the carrier (game) process once, then poll the handle.
    if (g.carrierProc == nullptr) {
        g.carrierProc = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (g.carrierProc == nullptr) {
            // Already gone, or inaccessible (unlikely in-prefix).
            return true;
        }
    }
    DWORD now = GetTickCount();
    if (now - g.lastDeathCheck < checkIntervalMs) return false;
    g.lastDeathCheck = now;
    if (WaitForSingleObject(g.carrierProc, 0) == WAIT_OBJECT_0) {
        if (!g.warnedCarrierDead) {
            hostlog::write("ipc: carrier (pid %u) exited — shutting down", pid);
            g.warnedCarrierDead = true;
        }
        return true;
    }
    return false;
}

void mark_exiting() {
    if (!enabled()) return;
    g.blk->hostExiting = 1;
}

}  // namespace ipc
