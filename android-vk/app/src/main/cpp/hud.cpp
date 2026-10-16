#include "hud.h"

#include <algorithm>
#include <cstring>

#include "diag.h"

namespace {

struct Frame {
    int w, h;
    std::vector<Vertex>* out;
    size_t limit;

    // Pixel rectangle [x0,x1) x [y0,y1) with the top left at (0,0), as two triangles.
    void rect(float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b) {
        if (out->size() + 6 > limit) return;
        const float nx0 = -1.0f + 2.0f * x0 / w, nx1 = -1.0f + 2.0f * x1 / w;
        const float ny0 = -1.0f + 2.0f * y0 / h, ny1 = -1.0f + 2.0f * y1 / h;
        const float xs[4] = {nx0, nx1, nx1, nx0};
        const float ys[4] = {ny0, ny0, ny1, ny1};
        const int order[6] = {0, 1, 2, 0, 2, 3};
        for (int k : order) {
            Vertex v;
            v.pos[0] = xs[k];
            v.pos[1] = ys[k];
            v.pos[2] = 0.5f;
            v.rgba[0] = r;
            v.rgba[1] = g;
            v.rgba[2] = b;
            v.rgba[3] = 255;
            setNormal(v, 0.0f, 1.0f, 0.0f);  // up: the shader's light then keeps the colour readable
            out->push_back(v);
        }
    }
};

}  // namespace

size_t buildHudText(const char* const* lines, int count, int w, int h, std::vector<Vertex>& out) {
    out.clear();
    if (w <= 0 || h <= 0 || count <= 0) return 0;
    Frame f{w, h, &out, kMaxHudVertices};

    const int s = std::max(3, h / 180);  // pixels per glyph dot
    const int pad = 2 * s;
    const int advance = 6 * s;           // 5 dots plus one gap
    const int lineH = 9 * s;             // 7 rows plus a gap
    int longest = 0;
    for (int i = 0; i < count; ++i) longest = std::max(longest, static_cast<int>(std::strlen(lines[i])));
    const float panelW = static_cast<float>(longest * advance + 2 * pad);
    const float panelH = static_cast<float>(count * lineH + 2 * pad);
    f.rect(0.0f, 0.0f, panelW, panelH, 16, 18, 24);

    for (int i = 0; i < count; ++i) {
        const char* text = lines[i];
        for (int c = 0; text[c] != '\0'; ++c) {
            const char* const* rows = glyphRows(text[c]);
            if (!rows) continue;
            const int gx = pad + c * advance;
            const int gy = pad + i * lineH;
            for (int r = 0; r < 7; ++r) {
                for (int k = 0; k < 5; ++k) {
                    if (rows[r][k] != '#') continue;
                    const float x0 = static_cast<float>(gx + k * s), y0 = static_cast<float>(gy + r * s);
                    f.rect(x0, y0, x0 + s, y0 + s, 255, 255, 255);
                }
            }
        }
    }
    return out.size();
}
