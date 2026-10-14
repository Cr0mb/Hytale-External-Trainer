// =============================================================================
//  viewproj.h — world -> normalized device coordinates with a client matrix.
//
//  Shared by the trainer (camera.cpp, external overlay) and the in-game
//  renderer DLL, so both project exactly the same way.
// =============================================================================
#pragma once
#include <cmath>

namespace vp {

// layout 0: clip = p * M (row-vector, DirectX style); layout 1: clip = M * p.
// origin is subtracted first (camera-relative matrices); zero otherwise.
inline bool ProjectNdc(const float m[16], int layout, const float origin[3],
                       float x, float y, float z, float& ndcX, float& ndcY) {
    const float px = x - origin[0], py = y - origin[1], pz = z - origin[2];
    float cx, cy, w;
    if (layout == 0) {
        cx = px * m[0] + py * m[4] + pz * m[8]  + m[12];
        cy = px * m[1] + py * m[5] + pz * m[9]  + m[13];
        w  = px * m[3] + py * m[7] + pz * m[11] + m[15];
    } else {
        cx = px * m[0]  + py * m[1]  + pz * m[2]  + m[3];
        cy = px * m[4]  + py * m[5]  + pz * m[6]  + m[7];
        w  = px * m[12] + py * m[13] + pz * m[14] + m[15];
    }
    if (!(w > 0.05f)) return false;               // behind the camera
    ndcX = cx / w;
    ndcY = cy / w;
    return std::isfinite(ndcX) && std::isfinite(ndcY);
}

// Cheap sanity check on a matrix read live: finite, and the forward row is a
// unit vector. Catches a freed or reused address without the full validation.
inline bool LooksValid(const float m[16], int layout) {
    for (int i = 0; i < 16; ++i)
        if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1e6f) return false;
    float a = layout == 0 ? m[3] : m[12], b = layout == 0 ? m[7] : m[13], c = layout == 0 ? m[11] : m[14];
    float l = a * a + b * b + c * c;
    return l > 0.94f && l < 1.07f;
}

// The camera centre in the matrix's own frame: the point where clip x, clip y
// and w are all zero. Zero for a matrix built around the camera; the boom
// offset for one built around the eye with the camera offset baked in.
inline bool SolveCenter(const float m[16], int layout, float out[3]) {
    float r[3][3], b[3];
    if (layout == 0) {
        r[0][0] = m[0]; r[0][1] = m[4]; r[0][2] = m[8];  b[0] = -m[12];
        r[1][0] = m[1]; r[1][1] = m[5]; r[1][2] = m[9];  b[1] = -m[13];
        r[2][0] = m[3]; r[2][1] = m[7]; r[2][2] = m[11]; b[2] = -m[15];
    } else {
        r[0][0] = m[0];  r[0][1] = m[1];  r[0][2] = m[2];  b[0] = -m[3];
        r[1][0] = m[4];  r[1][1] = m[5];  r[1][2] = m[6];  b[1] = -m[7];
        r[2][0] = m[12]; r[2][1] = m[13]; r[2][2] = m[14]; b[2] = -m[15];
    }
    auto det = [](float a[3][3]) {
        return a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1])
             - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0])
             + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
    };
    float d = det(r);
    if (!std::isfinite(d) || std::fabs(d) < 1e-9f) return false;
    for (int c = 0; c < 3; ++c) {
        float t[3][3];
        for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) t[i][j] = j == c ? b[i] : r[i][j];
        out[c] = det(t) / d;
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

} // namespace vp
