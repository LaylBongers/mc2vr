// Tiny header-only vector/quaternion math for the carrier. Plain value types, no allocation, no dependencies beyond
// <cmath>. Keeps camera/pose math out of hand-expanded x/y/z arithmetic.
#pragma once

#include <cmath>
#include <utility>

namespace mc2vr::math {

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// Unit quaternion, xyzw (OpenXR layout).
struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

inline Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 operator-(Vec3 a) { return {-a.x, -a.y, -a.z}; }
inline Vec3 operator*(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline Vec3 operator*(float s, Vec3 a) { return a * s; }

inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
// dot in double (no float rounding of the products or the sum).
inline double dot_d(Vec3 a, Vec3 b) { return (double)a.x * b.x + (double)a.y * b.y + (double)a.z * b.z; }
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
template <class T>
inline void store3(T *p, Vec3 v)
{
    p[0] = v.x;
    p[1] = v.y;
    p[2] = v.z;
}

// ---- 4x4 matrices: float[16], row-major (m[r*4+c]) ----

// out = a * b (out must not alias a or b).
inline void mul4(const float *a, const float *b, float *out)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r * 4 + c] = a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] + a[r * 4 + 2] * b[2 * 4 + c] +
                             a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
}

// out = m^-1 by Gauss-Jordan with partial pivoting, in double; false if
// (near-)singular. Perspective view-projections are ill-conditioned (their
// depth row is nearly the w row), so keep the result in double.
template <class T>
inline bool invert4(const T *m, double *out)
{
    double a[4][8];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            a[r][c] = m[r * 4 + c];
            a[r][c + 4] = r == c ? 1.0 : 0.0;
        }
    }
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++) {
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        }
        if (std::fabs(a[p][c]) < 1e-12) {
            return false;
        }
        if (p != c) {
            for (int k = 0; k < 8; k++) std::swap(a[p][k], a[c][k]);
        }
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            const double f = a[r][c];
            if (f != 0.0) {
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
        }
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) out[r * 4 + c] = a[r][c + 4];
    }
    return true;
}

// out = a * b in double (b, out: double; a: float or double).
template <class T>
inline void mul4(const T *a, const double *b, double *out)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r * 4 + c] = (double)a[r * 4 + 0] * b[0 * 4 + c] + (double)a[r * 4 + 1] * b[1 * 4 + c] +
                             (double)a[r * 4 + 2] * b[2 * 4 + c] + (double)a[r * 4 + 3] * b[3 * 4 + c];
        }
    }
}

// out = a * b with a in double, b in float, rounded once to float.
inline void mul4(const double *a, const float *b, float *out)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r * 4 + c] = (float)(a[r * 4 + 0] * b[0 * 4 + c] + a[r * 4 + 1] * b[1 * 4 + c] +
                                     a[r * 4 + 2] * b[2 * 4 + c] + a[r * 4 + 3] * b[3 * 4 + c]);
        }
    }
}

// out = m * v for a column 4-vector, in double (m: float or double).
template <class T>
inline void transform4(const T *m, const double v[4], double out[4])
{
    for (int r = 0; r < 4; r++) {
        out[r] = (double)m[r * 4 + 0] * v[0] + (double)m[r * 4 + 1] * v[1] + (double)m[r * 4 + 2] * v[2] +
                 (double)m[r * 4 + 3] * v[3];
    }
}

// out = m * (p, 1): a point through a row-form matrix, in double.
template <class T>
inline void transform_point4(const T *m, Vec3 p, double out[4])
{
    const double v[4] = {p.x, p.y, p.z, 1.0};
    transform4(m, v, out);
}

// out = S * m * S with S = diag(s): the matrix m expressed in a frame whose
// axes are scaled by s (for signs ±1, S^-1 = S). out may alias m.
inline void diag_conjugate4(const double *m, const double s[4], double *out)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) out[r * 4 + c] = s[r] * m[r * 4 + c] * s[c];
    }
}

// out = diag(s) * m: row r of m scaled by s[r]. out may alias m.
template <class T>
inline void scale_rows4(const double s[4], const T *m, T *out)
{
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) out[r * 4 + c] = (T)(s[r] * m[r * 4 + c]);
    }
}

inline void identity4(double *out)
{
    for (int i = 0; i < 16; i++) out[i] = i % 5 == 0 ? 1.0 : 0.0;
}

// out = m^-1 for a rigid m (orthonormal 3x3 R, translation t, last row
// 0 0 0 1): R^T and -R^T t, exact in closed form. out must not alias m.
inline void rigid_inverse4(const double *m, double *out)
{
    identity4(out);
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) out[r * 4 + c] = m[c * 4 + r];
        out[r * 4 + 3] = -(m[0 * 4 + r] * m[3] + m[1 * 4 + r] * m[7] + m[2 * 4 + r] * m[11]);
    }
}

// The 4x4 of an affine matrix given as its 3 rows (row 3 = 0 0 0 1).
inline void expand_affine4(const float *rows3, float *out)
{
    for (int i = 0; i < 12; i++) out[i] = rows3[i];
    out[12] = out[13] = out[14] = 0.0f;
    out[15] = 1.0f;
}

inline void to_float4(const double *m, float *out)
{
    for (int i = 0; i < 16; i++) out[i] = (float)m[i];
}

// Largest |a − ref| / (1 + |ref|) over n entries.
template <class A, class B>
inline double max_relative_diff(const A *a, const B *ref, int n)
{
    double worst = 0.0;
    for (int i = 0; i < n; i++) {
        const double e = std::fabs((double)a[i] - (double)ref[i]) / (1.0 + std::fabs((double)ref[i]));
        if (e > worst) worst = e;
    }
    return worst;
}

// Largest |a − b| over n entries.
template <class A, class B>
inline double max_abs_diff(const A *a, const B *b, int n)
{
    double worst = 0.0;
    for (int i = 0; i < n; i++) {
        const double e = std::fabs((double)a[i] - (double)b[i]);
        if (e > worst) worst = e;
    }
    return worst;
}

} // namespace mc2vr::math
