// Head-hiding bone controller — see head_hide.hpp.

#include "vr/head_hide.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>

#include "game/animation.hpp"
#include "hooks/frame_tick.hpp"
#include "log.hpp"
#include "vr/first_person.hpp"

namespace mc2vr::head_hide {

namespace {

// Engine controller layout: vtable, list node {next, prev, payload, key}, then ours.
struct Controller {
    void *const *vtbl;
    uint32_t next, prev;
    void *payload;
    int32_t key;
    uint32_t handle;  // the character it was attached to
};
static_assert(offsetof(Controller, next) == game::bone_controller::kNodeOff);
static_assert(offsetof(Controller, handle) == game::bone_controller::kSize);

// After the stock controllers (look-at etc.: priorities 10..25 seen), so the
// head is collapsed on the final pose.
constexpr int32_t kPriority = 1000;
constexpr uint32_t kTypeHash = 0x4d435648u;  // "MCVH"; no game type uses it

Controller *g_ctrl = nullptr;  // the attached one (update thread)
uint32_t g_attached = 0, g_failed = 0;
uint64_t g_retry_frame = 0;

// ---- slot bodies (thiscall, callee-popped like the engine's) ----
void __attribute__((thiscall)) slot_dtor(Controller *self, uint32_t flags)
{
    if (g_ctrl == self) g_ctrl = nullptr;  // the engine dropped it (character gone, or IsDone)
    if (flags & 1) HeapFree(GetProcessHeap(), 0, self);
}
void __attribute__((thiscall)) slot_update(Controller *, float /*dt*/, uint32_t /*object*/) {}
void __attribute__((thiscall)) slot_on_pose(Controller *self, const uint8_t *name_map, uint8_t *pose)
{
    first_person::on_head_pose(self->handle, name_map, pose);
}
bool __attribute__((thiscall)) slot_is_done(Controller *self) { return self != g_ctrl; }  // stale: delete it
int32_t __attribute__((thiscall)) slot_priority(Controller *) { return kPriority; }
uint32_t __attribute__((thiscall)) slot_type_hash(Controller *) { return kTypeHash; }

void *g_vtbl[game::bone_controller::kSlots];

void init_vtbl()
{
    using namespace game::bone_controller;
    for (void *&s : g_vtbl) s = (void *)kNoopRet4;  // 9..15 and 1: (1 arg) ret 4
    g_vtbl[0] = (void *)&slot_dtor;
    g_vtbl[2] = (void *)&slot_update;
    g_vtbl[3] = (void *)kNoopRet10;
    g_vtbl[4] = (void *)kNoopRet0;
    g_vtbl[5] = (void *)&slot_on_pose;
    g_vtbl[6] = (void *)&slot_is_done;
    g_vtbl[7] = (void *)&slot_priority;
    g_vtbl[8] = (void *)&slot_type_hash;
}

} // namespace

void attach(uint32_t handle)
{
    if (handle == 0 || (g_ctrl != nullptr && g_ctrl->handle == handle)) return;
    const uint64_t now = hooks::frame_count();
    if (now < g_retry_frame) return;
    if (g_vtbl[0] == nullptr) init_vtbl();
    g_ctrl = nullptr;  // a previous one (other character) is now stale: IsDone deletes it

    auto *c = static_cast<Controller *>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(Controller)));
    if (c == nullptr) return;
    c->vtbl = g_vtbl;
    c->handle = handle;
    g_ctrl = c;  // before the add: the add calls slot 7 (priority), and IsDone must see it as live
    if (!game::BoneControllerRuntime_AddController(handle, c)) {
        g_ctrl = nullptr;
        HeapFree(GetProcessHeap(), 0, c);
        g_failed++;
        g_retry_frame = now + 90;  // the character may not have its animated model yet
        if (g_failed <= 3) MC2VR_LOG("fp: head controller: add to character %08X failed (retrying)", handle);
        return;
    }
    g_attached++;
    MC2VR_LOG("fp: head controller %p attached to character %08X (priority %d)", (void *)c, handle, kPriority);
}

uint32_t attached_count() { return g_attached; }
uint32_t failed_count() { return g_failed; }

} // namespace mc2vr::head_hide
