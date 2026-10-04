// Plain pose types shared by the real and mock paths (the S4-1 IPC block
// will carry these).
#pragma once

struct Quat { float x = 0, y = 0, z = 0, w = 1; };
struct Vec3 { float x = 0, y = 0, z = 0; };

struct Fov { float left = 0, right = 0, up = 0, down = 0; };  // radians, left/down negative

struct EyePose {
    Vec3 pos;
    Quat rot;
    Fov fov;
};

struct HmdFrame {
    long long displayTime = 0;  // XrTime ns (mock: QPC-derived)
    EyePose eye[2];
    bool tracked = false;
};
