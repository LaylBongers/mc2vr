// First-person camera, increment 1 (docs/first_person.md): the on-foot camera
// (CamHuman) keeps computing its rotation, look input and aim mode as stock;
// in first person its final position is the character's eye point instead of
// the orbit boom, and its obstruction pull-in is off. Everything downstream —
// the working/output pose, g_CameraTable and the HMD injection, culling, the
// aim ray — follows from the camera's own pose. Head hiding, the head-driven
// body and room-scale placement are later increments.
//
// Eye point = the character's pose position (the pose store, via the camera's
// target handle) + eye height up + a small forward offset along the camera's
// horizontal forward (until head hiding, it keeps the view in front of the
// face rather than inside the head mesh).
#pragma once

#include <cstdint>

namespace mc2vr::first_person {

// mc2vr.conf first_person=on|off (default off). False on bad input.
bool set_enabled(const char *value);
// first_person_eye_height=<metres above the character's pose position> (default 1.65).
bool set_eye_height(const char *value);
// first_person_eye_forward=<metres ahead, along the view's horizontal forward> (default 0.15).
bool set_eye_forward(const char *value);

// first_person_hide_head=on|off (default off): attach the head controller
// (vr/head_hide.hpp) and collapse the head's local scale in its OnPose. The
// controller works, but the engine publishes bone poses without scale, so the
// head stays visible (docs/first_person.md, OPEN).
bool set_hide_head(const char *value);

bool enabled();

// From our bone controller's OnPose (update thread, AnimSystem_PoseTask):
// while first person is live on that character, collapses the head bone's
// subtree (local scale ~0, model space marked dirty for syncModelSpace).
void on_head_pose(uint32_t character, const uint8_t *name_map, uint8_t *pose);

// CamHuman_Update with its final world matrix ready (update thread;
// hooks/first_person.cpp). camera = the CamHuman object; matrix = the 4x4
// (rows = axes, translation at [12..14]) the camera turns into its working
// pose right after. Rewrites the translation in first person.
void on_human_camera_matrix(uint8_t *camera, float *matrix);

// 10 s window report (render_dump's poller).
void report_window();

} // namespace mc2vr::first_person
