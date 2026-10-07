// Tiny header-only vector/quaternion math shared by the carrier (i386) and the
// host (x86_64). Plain value types, no allocation, no dependencies beyond
// <cmath>. Keeps camera/pose math out of hand-expanded x/y/z arithmetic.
#pragma once

#include <cmath>

namespace mc2vr::math {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// Unit quaternion, xyzw (OpenXR / mc2_ipc layout).
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return a * s; }

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b)
{
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
inline float distance(Vec3 a, Vec3 b) { return length(a - b); }

// a / |a|; false (and `out` untouched) when |a| is too small to trust.
inline bool normalize(Vec3 a, Vec3 *out, float min_len = 1e-10f)
{
    const float len = length(a);
    if (!(len > min_len)) {
        return false;
    }
    *out = a * (1.0f / len);
    return true;
}

// `a` with its component along unit vector `n` removed.
inline Vec3 reject(Vec3 a, Vec3 n) { return a - n * dot(a, n); }

// Rotate v by unit quaternion q (q * v * q^-1).
inline Vec3 rotate(Quat q, Vec3 v)
{
    const Vec3 u = {q.x, q.y, q.z};
    const Vec3 t = cross(u, v) * 2.0f;
    return v + t * q.w + cross(u, t);
}

inline float norm_sq(Quat q) { return q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w; }

// Hamilton product: rotate(a * b, v) == rotate(a, rotate(b, v)).
inline Quat operator*(Quat a, Quat b)
{
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

inline Quat conj(Quat q) { return {-q.x, -q.y, -q.z, q.w}; }

// Read/write 3 consecutive floats (shader-constant rows are float[4], xyz first).
inline Vec3 load3(const float *p) { return {p[0], p[1], p[2]}; }
inline void store3(float *p, Vec3 v)
{
    p[0] = v.x;
    p[1] = v.y;
    p[2] = v.z;
}

} // namespace mc2vr::math
