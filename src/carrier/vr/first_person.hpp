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

bool enabled();

// CamHuman_Update with its final world matrix ready (update thread;
// hooks/first_person.cpp). camera = the CamHuman object; matrix = the 4x4
// (rows = axes, translation at [12..14]) the camera turns into its working
// pose right after. Rewrites the translation in first person.
void on_human_camera_matrix(uint8_t *camera, float *matrix);

// 10 s window report (render_dump's poller).
void report_window();

} // namespace mc2vr::first_person
