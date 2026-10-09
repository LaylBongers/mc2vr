// mc2vr self-test IPC probe (S4-1): a stand-in carrier for the host's --mock
// mode. Plays the carrier role from the IPC contract (docs/plans/stereo_design.md
// §S4): connects, validates the section, registers its pid, reads seqlocked
// poses, drains session-state events, sends Shutdown, and verifies the host
// exits cleanly. Win32 on purpose — it validates the cross-bitness path
// (win64 host <-> win32 client) exactly like the real carrier.
//
//   ipc_probe.exe --name <section-name> [--timeout ms]
//
// Exits 0 only if every step passes; prints one "[probe]" line per step.

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mc2vr_ipc.h"

static int fail(const char *what)
{
    printf("[probe] FAIL: %s (GLE=%lu)\n", what, GetLastError());
    return 1;
}

int main(int argc, char **argv)
{
    const char *name = NULL;
    DWORD timeout = 15000;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--name") && i + 1 < argc) name = argv[++i];
        else if (!strcmp(argv[i], "--timeout") && i + 1 < argc) timeout = atoi(argv[++i]);
        else {
            fprintf(stderr, "usage: ipc_probe --name NAME [--timeout ms]\n");
            return 2;
        }
    }
    if (!name) {
        fprintf(stderr, "usage: ipc_probe --name NAME [--timeout ms]\n");
        return 2;
    }

    char obj[256];
    snprintf(obj, sizeof obj, "Local\\%s", name);

    // 1. Open the section (host may still be starting).
    HANDLE section = NULL;
    for (DWORD waited = 0; waited < timeout && !section; waited += 50) {
        section = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, obj);
        if (!section) Sleep(50);
    }
    if (!section) return fail("section never appeared");
    printf("[probe] section opened\n");

    Mc2IpcBlock *blk = (Mc2IpcBlock *)MapViewOfFile(section, FILE_MAP_ALL_ACCESS,
                                                    0, 0, sizeof(Mc2IpcBlock));
    if (!blk) return fail("MapViewOfFile");
    if (blk->magic != MC2VR_IPC_MAGIC || blk->version != MC2VR_IPC_VERSION) {
        printf("[probe] FAIL: magic/version mismatch (%08x/%u)\n", blk->magic,
               blk->version);
        return 1;
    }
    if (blk->blockSize != sizeof(Mc2IpcBlock)) {
        printf("[probe] FAIL: block size mismatch (%u vs %u)\n", blk->blockSize,
               (unsigned)sizeof(Mc2IpcBlock));
        return 1;
    }
    printf("[probe] header ok (host pid %lu, block %u bytes)\n", blk->hostPid,
           blk->blockSize);

    // 2. Register as the carrier.
    if (!mc2_ipc_register_carrier(blk, GetCurrentProcessId())) {
        printf("[probe] FAIL: carrier slot already taken (pid %u)\n",
               blk->carrierPid);
        return 1;
    }
    printf("[probe] registered as carrier (pid %lu)\n", GetCurrentProcessId());

    // 3. Read seqlocked poses: need 3 tracked reads with advancing frames
    //    and plausible content (ipd > 0, nonzero orientation).
    Mc2IpcState st;
    uint32_t lastFrame = 0;
    int good = 0;
    for (DWORD waited = 0; waited < timeout && good < 3; waited += 20) {
        const int rc = mc2_ipc_state_read(blk, &st);
        if (rc == 0 && (st.flags & MC2VR_IPC_STF_TRACKED) &&
            st.hostFrame != lastFrame && st.ipd > 0.0f) {
            lastFrame = st.hostFrame;
            ++good;
            printf("[probe] pose read %d: frame %u t=%lld ipd=%.4f "
                   "L=(%.3f %.3f %.3f)\n",
                   good, st.hostFrame, (long long)st.displayTime, st.ipd,
                   st.eye[0].pos.x, st.eye[0].pos.y, st.eye[0].pos.z);
        } else {
            Sleep(20);
        }
    }
    if (good < 3) {
        printf("[probe] FAIL: only %d fresh tracked pose reads\n", good);
        return 1;
    }

    // 4. Drain the event ring: expect at least one session-state event and
    //    FOCUSED to be reached eventually (mock ramps VISIBLE -> FOCUSED).
    int sawStateEvent = 0, sawFocused = 0;
    for (DWORD waited = 0; waited < timeout && !sawFocused; waited += 20) {
        Mc2IpcMsg ev;
        while (mc2_ring_pop(&blk->events, &ev) == 0) {
            if (ev.type == MC2VR_MSG_SESSION_STATE) {
                sawStateEvent = 1;
                printf("[probe] event: session state %u\n", ev.a);
                if (ev.a == MC2VR_XR_SESSION_FOCUSED) sawFocused = 1;
            }
        }
        if (!sawFocused) Sleep(20);
    }
    if (!sawStateEvent) {
        printf("[probe] FAIL: no session-state events\n");
        return 1;
    }
    if (!sawFocused) {
        printf("[probe] FAIL: never reached FOCUSED\n");
        return 1;
    }

    // 5. S4-2 command shapes: CONFIG + FRAME_READY (handle 0 = "no texture",
    //    used by this stand-in; the real carrier sends live shared handles).
    //    The host must drain them without complaint and stay alive.
    Mc2IpcMsg m;
    memset(&m, 0, sizeof m);
    m.type = MC2VR_CMD_CONFIG;
    m.a = 2560; m.b = 1440; m.c = 22;  // w/h/D3DFMT_X8R8G8B8
    if (mc2_ring_push(&blk->commands, &m) != 0) {
        printf("[probe] FAIL: command ring full (CONFIG)\n");
        return 1;
    }
    memset(&m, 0, sizeof m);
    m.type = MC2VR_CMD_FRAME_READY;
    m.x = 42; m.y = 0; m.a = 0; m.b = 0; m.c = 2560; m.d = 1440;
    if (mc2_ring_push(&blk->commands, &m) != 0) {
        printf("[probe] FAIL: command ring full (FRAME_READY)\n");
        return 1;
    }
    printf("[probe] CONFIG + FRAME_READY(handle 0) queued\n");
    Sleep(300);  // give the host's drain loop a beat to process them

    // 6. Send Shutdown; the host must acknowledge by exiting (hostExiting
    //    flag + process death).
    memset(&m, 0, sizeof m);
    m.type = MC2VR_CMD_SHUTDOWN;
    if (mc2_ring_push(&blk->commands, &m) != 0) {
        printf("[probe] FAIL: command ring full\n");
        return 1;
    }
    printf("[probe] Shutdown queued\n");

    HANDLE host = OpenProcess(SYNCHRONIZE, FALSE, blk->hostPid);
    for (DWORD waited = 0; waited < timeout; waited += 20) {
        if (blk->hostExiting) break;
        if (host && WaitForSingleObject(host, 20) == WAIT_OBJECT_0) break;
        Sleep(20);
    }
    if (!blk->hostExiting) {
        printf("[probe] FAIL: host did not set hostExiting\n");
        return 1;
    }
    if (host) {
        if (WaitForSingleObject(host, 5000) != WAIT_OBJECT_0) {
            printf("[probe] FAIL: host flagged exit but process lingers\n");
            return 1;
        }
    }
    printf("[probe] PASS: full round trip (connect, poses, events, shutdown)\n");
    return 0;
}
