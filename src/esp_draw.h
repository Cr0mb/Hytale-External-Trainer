// =============================================================================
//  esp_draw.h — the ESP itself: boxes, labels, health bars, snaplines.
//
//  One implementation for both renderers: the external overlay (fallback)
//  and the in-game renderer DLL, which draws into the game's own frame.
//  Callers hand in a projector and entities with final world positions.
// =============================================================================
#pragma once
#include "imgui.h"
#include "viewproj.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>

namespace espdraw {

// Maps world positions to pixels in the target's coordinate space.
struct Projector {
    // client matrix
    bool  client = false;
    float m[16]{};
    int   layout = 0;
    float origin[3]{};
    float xSign = 1.f, ySign = 1.f;
    // server-view fallback
    float e[3]{}, r[3]{}, u[3]{}, f[3]{};
    float focal = 1.f;
    // viewport (game client area) in target pixels
    float cx = 0, cy = 0, halfW = 0, halfH = 0;

    // Builds the fallback basis from the server rotation (radians; forward =
    // (-sin yaw cos pitch, sin pitch, -cos yaw cos pitch), Y up).
    void SetServerView(const float eye[3], float pitch, float yaw, float fovDegVertical) {
        client = false;
        e[0] = eye[0]; e[1] = eye[1]; e[2] = eye[2];
        float cp = std::cos(pitch), sp = std::sin(pitch), cyw = std::cos(yaw), syw = std::sin(yaw);
        f[0] = -syw * cp; f[1] = sp; f[2] = -cyw * cp;
        r[0] = cyw; r[1] = 0; r[2] = -syw;
        u[0] = r[1] * f[2] - r[2] * f[1];
        u[1] = r[2] * f[0] - r[0] * f[2];
        u[2] = r[0] * f[1] - r[1] * f[0];
        focal = halfH / std::tan(fovDegVertical * 3.14159265f / 360.f);
    }

    bool operator()(float x, float y, float z, ImVec2& out) const {
        if (client) {
            float nx, ny;
            if (!vp::ProjectNdc(m, layout, origin, x, y, z, nx, ny)) return false;
            out.x = cx + xSign * nx * halfW;
            out.y = cy - ySign * ny * halfH;
            return true;
        }
        float dx = x - e[0], dy = y - e[1], dz = z - e[2];
        float zc = dx * f[0] + dy * f[1] + dz * f[2];
        if (zc < 0.1f) return false;
        float xc = dx * r[0] + dy * r[1] + dz * r[2];
        float yc = dx * u[0] + dy * u[1] + dz * u[2];
        out.x = cx + xc / zc * focal;
        out.y = cy - yc / zc * focal;
        return true;
    }
};

struct Options {
    bool  hostile = true, neutral = true, friendly = false, players = true, items = false;
    bool  lines = false, info = true, health = true;
    float maxDist = 64.f;
};

struct Ent {
    char        kind;      // 'H' 'N' 'F' 'P' 'I'
    float       pos[3];    // feet, final (already interpolated)
    float       height, hp, maxHp;
    const char* name;
};

// Moves each entity onto the client's rendered position of the same entity:
// the nearest client object within 1.5 blocks, each used once. me likewise
// (within 2.5 blocks, the client is ahead of the server while you move).
// Returns how many entities were matched.
inline int MatchClient(Ent* ents, int n, const float (*cpos)[3], int m, float me[3], bool& meMatched) {
    static bool used[1024];
    m = m > 1024 ? 1024 : m;
    for (int j = 0; j < m; ++j) used[j] = false;
    auto nearest = [&](const float* p, float maxD) {
        int best = -1;
        float bd = maxD * maxD;
        for (int j = 0; j < m; ++j) {
            if (used[j]) continue;
            float dx = cpos[j][0] - p[0], dy = cpos[j][1] - p[1], dz = cpos[j][2] - p[2];
            float d = dx * dx + dy * dy + dz * dz;
            if (d < bd) { bd = d; best = j; }
        }
        return best;
    };
    meMatched = false;
    int j = nearest(me, 2.5f);
    if (j >= 0) { used[j] = true; me[0] = cpos[j][0]; me[1] = cpos[j][1]; me[2] = cpos[j][2]; meMatched = true; }
    int matched = 0;
    for (int i = 0; i < n; ++i) {
        j = nearest(ents[i].pos, 1.5f);
        if (j < 0) continue;
        used[j] = true;
        ents[i].pos[0] = cpos[j][0]; ents[i].pos[1] = cpos[j][1]; ents[i].pos[2] = cpos[j][2];
        ++matched;
    }
    return matched;
}

// targetW/H: size of the whole draw target (for culling); snapFrom: where
// snaplines start (bottom centre of the game view).
inline void Draw(ImDrawList* dl, ImFont* font, const Projector& proj, const Options& o,
                 const float me[3], const Ent* ents, int count,
                 float targetW, float targetH, ImVec2 snapFrom) {
    const float fs = 13.f, lh = 13.f;
    for (int i = 0; i < count; ++i) {
        const Ent& e = ents[i];
        ImU32 col;
        switch (e.kind) {
        case 'H': if (!o.hostile) continue;  col = IM_COL32(255, 77, 77, 255); break;
        case 'N': if (!o.neutral) continue;  col = IM_COL32(255, 200, 60, 255); break;
        case 'F': if (!o.friendly) continue; col = IM_COL32(70, 230, 110, 255); break;
        case 'P': if (!o.players) continue;  col = IM_COL32(235, 130, 255, 255); break;
        default:  if (!o.items) continue;    col = IM_COL32(120, 190, 255, 255); break;
        }
        const float* p = e.pos;
        float dx = p[0] - me[0], dy = p[1] - me[1], dz = p[2] - me[2];
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > o.maxDist) continue;

        if (e.kind == 'I') {
            ImVec2 sp;
            if (!proj(p[0], p[1] + 0.25f, p[2], sp)) continue;
            const float r = 4.f;
            ImVec2 pts[4] = { {sp.x, sp.y - r}, {sp.x + r, sp.y}, {sp.x, sp.y + r}, {sp.x - r, sp.y} };
            dl->AddConvexPolyFilled(pts, 4, (col & 0x00FFFFFF) | 0xC0000000);
            dl->AddPolyline(pts, 4, 0xE0000000, ImDrawFlags_Closed, 1.f);
            if (o.lines) dl->AddLine(snapFrom, sp, (col & 0x00FFFFFF) | 0x40000000, 1.f);
            if (o.info) {
                char lbl[96];
                if (e.hp > 1) snprintf(lbl, sizeof(lbl), "%s x%.0f  %.0fm", e.name, e.hp, dist);
                else snprintf(lbl, sizeof(lbl), "%s  %.0fm", e.name, dist);
                ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0, lbl);
                ImVec2 tp(sp.x - sz.x * 0.5f, sp.y - r - 2 - lh);
                dl->AddText(font, fs, ImVec2(tp.x + 1, tp.y + 1), 0xE0000000, lbl);
                dl->AddText(font, fs, tp, col, lbl);
            }
            continue;
        }

        ImVec2 feet, head;
        if (!proj(p[0], p[1], p[2], feet) || !proj(p[0], p[1] + e.height, p[2], head)) continue;
        float h = std::fabs(feet.y - head.y);
        if (h < 4.f) h = 4.f;
        float w = h * 0.5f;
        float top = (std::min)(feet.y, head.y);
        ImVec2 a(feet.x - w * 0.5f, top), b(feet.x + w * 0.5f, top + h);
        if (b.x < 0 || a.x > targetW || b.y < 0 || a.y > targetH) continue;

        if (o.lines) dl->AddLine(snapFrom, ImVec2(feet.x, b.y), (col & 0x00FFFFFF) | 0x66000000, 1.2f);
        dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), 0x90000000, 0.f, 0, 1.f);
        dl->AddRect(a, b, col, 0.f, 0, 1.4f);

        if (o.health && e.maxHp > 0) {
            float f = (std::max)(0.f, (std::min)(1.f, e.hp / e.maxHp));
            ImVec2 ba(a.x - 5, a.y), bb(a.x - 2, b.y);
            dl->AddRectFilled(ba, bb, 0xC0000000);
            dl->AddRectFilled(ImVec2(ba.x, bb.y - (bb.y - ba.y) * f), bb, ImGui::GetColorU32(ImVec4(1.f - f, f, 0.15f, 1.f)));
        }
        if (o.info) {
            char l1[96], l2[48];
            snprintf(l1, sizeof(l1), "%s", e.name);
            if (e.maxHp > 0) snprintf(l2, sizeof(l2), "%.0f HP  %.0fm", e.hp, dist);
            else             snprintf(l2, sizeof(l2), "%.0fm", dist);
            const char* lines[2] = { l1, l2 };
            float ty0 = a.y - 3 - 2 * lh;
            for (int k = 0; k < 2; ++k) {
                ImVec2 sz = font->CalcTextSizeA(fs, FLT_MAX, 0, lines[k]);
                ImVec2 tp(feet.x - sz.x * 0.5f, ty0 + k * lh);
                dl->AddText(font, fs, ImVec2(tp.x + 1, tp.y + 1), 0xE0000000, lines[k]);
                dl->AddText(font, fs, tp, k == 0 ? col : 0xFFCFCFCF, lines[k]);
            }
        }
    }
}

} // namespace espdraw
