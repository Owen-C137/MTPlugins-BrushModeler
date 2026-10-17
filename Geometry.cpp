// Brush and patch reconstruction for BrushModeler.
#include "Geometry.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace bm
{
namespace
{
std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
std::string Trim(const std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}
} // namespace

std::unordered_map<std::string, std::string> SplitBrushBlocks(const std::string& text)
{
    // Depth 1 is the entity block, depth 2 a brush or patch. Braces stand on their own lines.
    std::unordered_map<std::string, std::string> blocks;
    int depth = 0;
    bool inBlock = false;
    std::string current, guid;
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find('\n', pos);
        std::string raw = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        std::string line = Trim(raw);
        if (line == "{")
        {
            ++depth;
            if (depth == 2)
            {
                inBlock = true;
                current.clear();
                guid.clear();
            }
            else if (inBlock)
                current += raw + "\n";
            continue;
        }
        if (line == "}")
        {
            if (depth == 2 && inBlock)
            {
                if (!guid.empty()) blocks[guid] = current;
                inBlock = false;
            }
            else if (inBlock)
                current += raw + "\n";
            --depth;
            continue;
        }
        if (!inBlock) continue;
        if (guid.empty() && line.rfind("guid \"", 0) == 0)
        {
            size_t end = line.find('"', 6);
            if (end != std::string::npos) guid = Lower(line.substr(6, end - 6));
        }
        current += raw + "\n";
    }
    return blocks;
}

void ParseTex(const std::string& text, std::vector<Tex>& out)
{
    size_t p = 0;
    while (p < text.size())
    {
        size_t e = text.find('\n', p);
        std::string line = text.substr(p, e == std::string::npos ? e : e - p);
        p = e == std::string::npos ? text.size() : e + 1;
        size_t s = line.find_first_not_of(" \t\r");
        if (s == std::string::npos || line[s] != '(') continue;
        Tex t;
        int used = 0, used2 = 0;
        char name[256]{};
        const char* c = line.c_str() + s;
        if (sscanf_s(c, " ( %f %f %f ) ( %f %f %f ) ( %f %f %f ) %n", &t.p[0].x, &t.p[0].y, &t.p[0].z, &t.p[1].x, &t.p[1].y, &t.p[1].z,
                     &t.p[2].x, &t.p[2].y, &t.p[2].z, &used) < 9 ||
            used <= 0)
            continue;
        if (sscanf_s(c + used, "%255s %f %f %f %f %f%n", name, (unsigned)_countof(name), &t.hs, &t.vs, &t.hshift, &t.vshift, &t.rot,
                     &used2) < 6)
            continue;
        t.material = name;
        out.push_back(t);
    }
}

bool ParsePatch(const std::string& text, const char* guid, PatchGrid& out)
{
    out = PatchGrid{};
    if (!guid || !*guid) return false;
    std::string lower = Lower(text), needle = Lower(std::string("guid \"") + guid + "\"");
    size_t pos = lower.find(needle);
    if (pos == std::string::npos) return false;
    bool in = false, header = false;
    std::string lightmap;
    while (pos < text.size())
    {
        size_t e = text.find('\n', pos);
        std::string line = text.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        pos = e == std::string::npos ? text.size() : e + 1;
        size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) continue;
        line.erase(0, b);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        if (line.rfind("guid", 0) == 0) continue;
        if (!in)
        {
            if (line == "mesh" || line == "curve")
            {
                in = true;
                out.curve = line == "curve";
                continue;
            }
            return false;
        }
        if (line[0] == '{') continue;
        if (line[0] == '}') break;
        if (line[0] == '(' || line[0] == ')' || line.rfind("toolFlags", 0) == 0 || line.rfind("smoothing", 0) == 0) continue;
        if (line[0] == 'v' && line.size() > 1 && (line[1] == ' ' || line[1] == '\t'))
        {
            Vec p{};
            float u = 0, v = 0;
            if (sscanf_s(line.c_str(), "v %f %f %f t %f %f", &p.x, &p.y, &p.z, &u, &v) != 5) return false;
            out.p.push_back(p);
            out.uv.push_back({u / 1024.0f, v / 1024.0f});
            continue;
        }
        if (!header)
        {
            if (out.material.empty())
            {
                out.material = line;
                continue;
            }
            if (lightmap.empty())
            {
                lightmap = line;
                continue;
            }
            int r = 0, c = 0;
            if (sscanf_s(line.c_str(), "%d %d", &r, &c) == 2 && r >= 2 && c >= 2 && r <= 1024 && c <= 1024 &&
                (size_t)r * (size_t)c <= 1048576)
            {
                out.rows = r;
                out.cols = c;
                header = true;
                continue;
            }
            return false;
        }
    }
    if (!header || out.p.size() != (size_t)out.rows * (size_t)out.cols || out.uv.size() != out.p.size()) return false;
    return true;
}

bool TessellatePatchCurve(const PatchGrid& g, PatchGrid& out)
{
    if (!g.curve || g.rows < 3 || g.cols < 3 || (g.rows & 1) == 0 || (g.cols & 1) == 0) return false;
    int pr = (g.rows - 1) / 2, pc = (g.cols - 1) / 2;
    auto at = [&](int r, int c) -> size_t { return (size_t)r * g.cols + c; };
    auto bend = [](Vec a, Vec b, Vec c) {
        Vec d = Sub(Add(a, c), Mul(b, 2));
        return Dot(d, d);
    };
    float br = 0, bc = 0;
    for (int i = 0; i < pr; ++i)
        for (int c = 0; c < g.cols; ++c) br = (std::max)(br, bend(g.p[at(2 * i, c)], g.p[at(2 * i + 1, c)], g.p[at(2 * i + 2, c)]));
    for (int j = 0; j < pc; ++j)
        for (int r = 0; r < g.rows; ++r) bc = (std::max)(bc, bend(g.p[at(r, 2 * j)], g.p[at(r, 2 * j + 1)], g.p[at(r, 2 * j + 2)]));
    auto seg = [](float b) {
        int n = 2;
        while (n < 8 && sqrtf(b) * .25f / (n * n) > .25f) n *= 2;
        return n;
    };
    int nr = seg(br), nc = seg(bc);
    while (pr * nr * pc * nc > 512 && (nr > 1 || nc > 1))
    {
        if (pr * nr >= pc * nc && nr > 1)
            nr /= 2;
        else if (nc > 1)
            nc /= 2;
        else
            nr /= 2;
    }
    out = PatchGrid{};
    out.rows = pr * nr + 1;
    out.cols = pc * nc + 1;
    out.material = g.material;
    out.p.resize((size_t)out.rows * out.cols);
    out.uv.resize(out.p.size());
    for (int R = 0; R < out.rows; ++R)
    {
        int i = (std::min)(R / nr, pr - 1);
        float t = (float)(R - i * nr) / nr, wr[3] = {(1 - t) * (1 - t), 2 * t * (1 - t), t * t};
        for (int C = 0; C < out.cols; ++C)
        {
            int j = (std::min)(C / nc, pc - 1);
            float u = (float)(C - j * nc) / nc, wc[3] = {(1 - u) * (1 - u), 2 * u * (1 - u), u * u};
            Vec p{};
            float us = 0, vs = 0;
            for (int a = 0; a < 3; ++a)
                for (int b = 0; b < 3; ++b)
                {
                    float w = wr[a] * wc[b];
                    size_t src = at(2 * i + a, 2 * j + b);
                    p = Add(p, Mul(g.p[src], w));
                    us += g.uv[src].first * w;
                    vs += g.uv[src].second * w;
                }
            size_t d = (size_t)R * out.cols + C;
            out.p[d] = p;
            out.uv[d] = {us, vs};
        }
    }
    return true;
}

bool PatchFaces(const PatchGrid& src, std::vector<Face>& out)
{
    PatchGrid tess = src;
    if (src.curve && !TessellatePatchCurve(src, tess)) return false;
    if (tess.rows < 2 || tess.cols < 2 || tess.p.size() != (size_t)tess.rows * tess.cols || tess.uv.size() != tess.p.size()) return false;
    // One shift for the whole patch keeps neighbouring triangles' shared corners identical.
    RebaseUVs(tess.uv);
    auto idx = [&](int r, int c) { return (size_t)r * tess.cols + c; };
    std::vector<std::array<size_t, 3>> triangles;
    triangles.reserve((size_t)(tess.rows - 1) * (tess.cols - 1) * 2);
    std::vector<Vec> normalSums(tess.p.size(), Vec{});
    for (int r = 0; r < tess.rows - 1; ++r)
        for (int c = 0; c < tess.cols - 1; ++c)
        {
            size_t a = idx(r, c), b = idx(r, c + 1), d = idx(r + 1, c), e = idx(r + 1, c + 1);
            triangles.push_back({a, d, e});
            triangles.push_back({a, e, b});
        }
    for (const auto& t : triangles)
    {
        Vec n = Cross(Sub(tess.p[t[1]], tess.p[t[0]]), Sub(tess.p[t[2]], tess.p[t[0]]));
        if (Len(n) < 1e-6f) continue;
        for (size_t i : t) normalSums[i] = Add(normalSums[i], n);
    }
    for (Vec& n : normalSums)
    {
        float l = Len(n);
        if (l > 1e-6f) n = Mul(n, 1.0f / l);
    }
    for (const auto& t : triangles)
    {
        Vec p0 = tess.p[t[0]], p1 = tess.p[t[1]], p2 = tess.p[t[2]], n = Cross(Sub(p1, p0), Sub(p2, p0));
        float l = Len(n);
        if (l < 1e-6f) continue;
        Face f;
        f.p = {p0, p1, p2};
        f.n = Mul(n, 1.0f / l);
        f.material = tess.material;
        f.uv = {tess.uv[t[0]], tess.uv[t[1]], tess.uv[t[2]]};
        for (size_t i : t) f.normals.push_back(Len(normalSums[i]) > 1e-6f ? normalSums[i] : f.n);
        f.reverseWinding = true;
        out.push_back(std::move(f));
    }
    return true;
}

namespace
{
void FaceAxes(Vec n, const Tex& t, Vec& u, Vec& v)
{
    static const float a[18][3] = {{0, 0, 1},  {1, 0, 0}, {0, -1, 0}, {0, 0, -1}, {1, 0, 0}, {0, -1, 0}, {1, 0, 0},  {0, 1, 0}, {0, 0, -1},
                                   {-1, 0, 0}, {0, 1, 0}, {0, 0, -1}, {0, 1, 0},  {1, 0, 0}, {0, 0, -1}, {0, -1, 0}, {1, 0, 0}, {0, 0, -1}};
    int best = 0;
    float bd = -1e30f;
    for (int i = 0; i < 6; ++i)
    {
        float d = n.x * a[i * 3][0] + n.y * a[i * 3][1] + n.z * a[i * 3][2];
        if (d > bd)
        {
            best = i;
            bd = d;
        }
    }
    Vec x = {a[best * 3 + 1][0], a[best * 3 + 1][1], a[best * 3 + 1][2]}, y = {a[best * 3 + 2][0], a[best * 3 + 2][1], a[best * 3 + 2][2]};
    if (t.rot != 0)
    {
        float r = t.rot * 3.14159265358979f / 180, si = sin(r), co = cos(r);
        int sx = x.x ? 0 : x.y ? 1 : 2, sy = y.x ? 0 : y.y ? 1 : 2;
        float *xv = &x.x, *yv = &y.x;
        float nx = co * xv[sx] - si * xv[sy], ny = si * xv[sx] + co * xv[sy];
        xv[sx] = nx;
        xv[sy] = ny;
        nx = co * yv[sx] - si * yv[sy];
        ny = si * yv[sx] + co * yv[sy];
        yv[sx] = nx;
        yv[sy] = ny;
    }
    u = x;
    v = y;
}
std::pair<float, float> UV(Vec p, Vec u, Vec v, const Tex& t)
{
    float hs = fabsf(t.hs) < 1e-6f ? 64 : t.hs, vs = fabsf(t.vs) < 1e-6f ? 64 : t.vs;
    return {(Dot(p, u) + t.hshift) / hs, (Dot(p, v) + t.vshift) / vs};
}
struct Plane
{
    Vec n;
    float d;
    size_t raw;
};
} // namespace

bool Reconstruct(const float mins[3], const float maxs[3], const std::vector<float>& raw, const std::vector<Tex>& tex,
                 std::vector<Face>& out, size_t* untexturedFaces)
{
    if (raw.size() < 36 || raw.size() % 9) return false;
    std::vector<Plane> planes;
    for (size_t i = 0; i < raw.size() / 9; ++i)
    {
        Vec a = {raw[i * 9], raw[i * 9 + 1], raw[i * 9 + 2]}, b = {raw[i * 9 + 3], raw[i * 9 + 4], raw[i * 9 + 5]},
            c = {raw[i * 9 + 6], raw[i * 9 + 7], raw[i * 9 + 8]}, n = Cross(Sub(b, a), Sub(c, a));
        float l = Len(n);
        if (l < 1e-5f) continue;
        n = Mul(n, 1 / l);
        float d = Dot(n, a);
        bool dup = false;
        for (auto& p : planes)
            if (Dot(p.n, n) > .99999f && fabsf(p.d - d) < .03125f)
            {
                dup = true;
                break;
            }
        if (!dup) planes.push_back({n, d, i});
    }
    if (planes.size() < 4) return false;

    // Texture lines' own planes, for the fallback match when their points differ from Radiant's plane points.
    struct TexPlane
    {
        Vec n{};
        float d = 0;
        bool valid = false;
    };
    std::vector<TexPlane> texPlanes(tex.size());
    for (size_t i = 0; i < tex.size(); ++i)
    {
        Vec n = Cross(Sub(tex[i].p[1], tex[i].p[0]), Sub(tex[i].p[2], tex[i].p[0]));
        float l = Len(n);
        if (l < 1e-6f) continue;
        n = Mul(n, 1 / l);
        texPlanes[i] = {n, Dot(n, tex[i].p[0]), true};
    }

    for (int flip = 0; flip < 2; ++flip)
    {
        std::vector<Plane> ps = planes;
        if (flip)
            for (auto& p : ps)
            {
                p.n = Mul(p.n, -1);
                p.d = -p.d;
            }
        std::vector<Vec> verts;
        for (size_t i = 0; i < ps.size(); ++i)
            for (size_t j = i + 1; j < ps.size(); ++j)
                for (size_t k = j + 1; k < ps.size(); ++k)
                {
                    auto& a = ps[i];
                    auto& b = ps[j];
                    auto& c = ps[k];
                    Vec bc = Cross(b.n, c.n);
                    float den = Dot(a.n, bc);
                    if (fabsf(den) < 1e-5f) continue;
                    Vec x = Mul(Add(Add(Mul(bc, a.d), Mul(Cross(c.n, a.n), b.d)), Mul(Cross(a.n, b.n), c.d)), 1 / den);
                    bool inside = true;
                    for (auto& p : ps)
                        if (Dot(p.n, x) > p.d + .125f)
                        {
                            inside = false;
                            break;
                        }
                    if (!inside || x.x < mins[0] - .5f || x.x > maxs[0] + .5f || x.y < mins[1] - .5f || x.y > maxs[1] + .5f ||
                        x.z < mins[2] - .5f || x.z > maxs[2] + .5f)
                        continue;
                    bool du = false;
                    for (auto& v : verts)
                        if (Dot(Sub(v, x), Sub(v, x)) < .001f)
                        {
                            du = true;
                            break;
                        }
                    if (!du) verts.push_back(x);
                }
        if (verts.size() < 4) continue;
        std::vector<Face> made;
        size_t untextured = 0;
        for (auto& p : ps)
        {
            std::vector<Vec> corners;
            Vec center{};
            for (auto& v : verts)
                if (fabsf(Dot(p.n, v) - p.d) < .15625f)
                {
                    corners.push_back(v);
                    center = Add(center, v);
                }
            if (corners.size() < 3) continue;
            center = Mul(center, 1.0f / corners.size());
            Vec u = Sub(corners[0], center);
            u = Mul(u, 1 / std::max(1e-5f, Len(u)));
            Vec v = Cross(p.n, u);
            std::sort(corners.begin(), corners.end(), [&](Vec a, Vec b) {
                Vec aa = Sub(a, center), bb = Sub(b, center);
                return atan2f(Dot(aa, v), Dot(aa, u)) < atan2f(Dot(bb, v), Dot(bb, u));
            });
            Vec area{};
            for (size_t i = 1; i + 1 < corners.size(); ++i)
                area = Add(area, Cross(Sub(corners[i], corners[0]), Sub(corners[i + 1], corners[0])));
            if (Dot(area, p.n) < 0) std::reverse(corners.begin(), corners.end());
            Face f;
            f.p = corners;
            f.n = p.n;
            const Tex* t = nullptr;
            auto close = [](Vec a, Vec b) {
                Vec d = Sub(a, b);
                return Dot(d, d) < .01f;
            };
            Vec ra = {raw[p.raw * 9], raw[p.raw * 9 + 1], raw[p.raw * 9 + 2]},
                rb = {raw[p.raw * 9 + 3], raw[p.raw * 9 + 4], raw[p.raw * 9 + 5]},
                rc = {raw[p.raw * 9 + 6], raw[p.raw * 9 + 7], raw[p.raw * 9 + 8]};
            for (auto& tx : tex)
                if (close(tx.p[0], ra) && close(tx.p[1], rb) && close(tx.p[2], rc))
                {
                    t = &tx;
                    break;
                }
            Vec rawN = Cross(Sub(rb, ra), Sub(rc, ra));
            float rawLen = Len(rawN);
            if (rawLen > 1e-6f) rawN = Mul(rawN, 1.0f / rawLen);
            if (!t && rawLen > 1e-6f)
            {
                float rawD = Dot(rawN, ra);
                for (size_t i = 0; i < tex.size(); ++i)
                    if (texPlanes[i].valid && Dot(texPlanes[i].n, rawN) > .9999f && fabsf(texPlanes[i].d - rawD) < .05f)
                    {
                        t = &tex[i];
                        break;
                    }
            }
            if (t)
            {
                f.material = t->material;
                Vec tu, tv;
                FaceAxes(rawN, *t, tu, tv);
                for (auto& pt : corners) f.uv.push_back(UV(pt, tu, tv, *t));
                RebaseUVs(f.uv);
            }
            else
            {
                f.material = "caulk";
                ++untextured;
                for (size_t i = 0; i < corners.size(); ++i) f.uv.push_back({0.0f, 0.0f});
            }
            made.push_back(std::move(f));
        }
        if (made.size() >= 4)
        {
            out.insert(out.end(), made.begin(), made.end());
            if (untexturedFaces) *untexturedFaces += untextured;
            return true;
        }
    }
    return false;
}

bool IsToolMaterial(const std::string& material)
{
    std::string m = Lower(material);
    static const char* exact[] = {"aitrig", "hint", "occluder", "occlusion", "origin", "shadowcaster", "skip", "sky", "traverse"};
    for (const char* e : exact)
        if (m == e) return true;
    // A family name alone or followed by "_": "clip", "clip_player"... but not "clipboard_paper".
    static const char* families[] = {"caulk",  "clip",      "nodraw", "portal",  "trigger", "volume",
                                     "ladder", "lightgrid", "mantle", "navmesh", "umbra"};
    for (const char* f : families)
    {
        size_t n = strlen(f);
        if (m.compare(0, n, f) == 0 && (m.size() == n || m[n] == '_')) return true;
    }
    return false;
}

void RebaseUVs(std::vector<std::pair<float, float>>& uv)
{
    if (uv.empty()) return;
    float mu = uv[0].first, mv = uv[0].second;
    for (const auto& t : uv)
    {
        mu = (std::min)(mu, t.first);
        mv = (std::min)(mv, t.second);
    }
    float su = std::floor(mu), sv = std::floor(mv);
    if (!std::isfinite(su) || !std::isfinite(sv) || (su == 0 && sv == 0)) return;
    for (auto& t : uv)
    {
        t.first -= su;
        t.second -= sv;
    }
}

std::string FormatFloat(float v)
{
    if (!std::isfinite(v)) return "0";
    if (v == 0) return "0";
    // Fixed notation keeps the text plain for export2bin; the digits are still the shortest exact ones.
    char buf[64];
    auto r = std::to_chars(buf, buf + sizeof buf, v, std::chars_format::fixed);
    if (r.ec != std::errc()) return "0";
    return std::string(buf, r.ptr);
}

std::string BuildXModelExport(const std::vector<Face>& faces, Vec pivot, ExportStats& stats)
{
    std::vector<std::string> mats;
    for (const auto& f : faces)
        if (std::find(mats.begin(), mats.end(), f.material) == mats.end()) mats.push_back(f.material);
    size_t nv = 0, nf = 0;
    for (const auto& f : faces)
    {
        nv += (f.p.size() - 2) * 3;
        nf += f.p.size() - 2;
    }
    stats = {nv, nf, mats.size()};
    std::string o;
    o.reserve(nv * 120 + 4096);
    o += "MODEL\nVERSION 6\n\nNUMBONES 1\nBONE 0 -1 \"tag_origin\"\n\nBONE 0\nOFFSET 0 0 0\nSCALE 1 1 1\nX 1 0 0\nY 0 1 0\nZ 0 0 "
         "1\n\nNUMVERTS ";
    o += std::to_string(nv) + "\n\n";
    // CoD xmodel_export brush faces use reversed winding; patches retain Radiant's visible-side order.
    auto corner = [](const Face& f, size_t k, int q) {
        return q == 0 ? 0 : q == 1 ? (f.reverseWinding ? k + 1 : k) : (f.reverseWinding ? k : k + 1);
    };
    size_t vi = 0;
    for (const auto& f : faces)
        for (size_t k = 1; k + 1 < f.p.size(); ++k)
            for (int q = 0; q < 3; ++q)
            {
                Vec p = Sub(f.p[corner(f, k, q)], pivot);
                o += "VERT " + std::to_string(vi++) + "\nOFFSET " + FormatFloat(p.x) + " " + FormatFloat(p.y) + " " + FormatFloat(p.z) +
                     "\nBONES 1\nBONE 0 1\n\n";
            }
    o += "NUMFACES " + std::to_string(nf) + "\n\n";
    vi = 0;
    for (const auto& f : faces)
    {
        size_t mi = std::find(mats.begin(), mats.end(), f.material) - mats.begin();
        for (size_t k = 1; k + 1 < f.p.size(); ++k)
        {
            o += "TRI 0 " + std::to_string(mi) + " 0 0\n";
            for (int q = 0; q < 3; ++q)
            {
                size_t ci = corner(f, k, q);
                Vec normal = f.normals.size() == f.p.size() ? f.normals[ci] : f.n;
                o += "VERT " + std::to_string(vi++) + "\nNORMAL " + FormatFloat(normal.x) + " " + FormatFloat(normal.y) + " " +
                     FormatFloat(normal.z) + "\nCOLOR 1 1 1 1\nUV 1 " + FormatFloat(f.uv[ci].first) + " " + FormatFloat(f.uv[ci].second) +
                     "\n\n";
            }
        }
    }
    o += "NUMOBJECTS 1\nOBJECT 0 \"selection\"\n\nNUMMATERIALS " + std::to_string(mats.size()) + "\n";
    for (size_t i = 0; i < mats.size(); ++i)
        o += "MATERIAL " + std::to_string(i) + " \"" + mats[i] +
             "\" \"Lambert\" \"\"\nCOLOR 0 0 0 1\nTRANSPARENCY 0 0 0 1\nAMBIENTCOLOR 1 1 1 1\nINCANDESCENCE 0 0 0 1\nCOEFFS 0.8 0\nGLOW 0 "
             "0\nREFRACTIVE 6 1\nSPECULARCOLOR 0.5 0.5 0.5 1\nREFLECTIVECOLOR 0 0 0 1\nREFLECTIVE 1 0.5\nBLINN -1 -1\nPHONG -1\n";
    return o;
}

std::string EmptyXModelExport()
{
    return "MODEL\nVERSION 6\n\nNUMBONES 1\nBONE 0 -1 \"tag_origin\"\n\nBONE 0\nOFFSET 0 0 0\nSCALE 1 1 1\nX 1 0 0\nY 0 1 0\nZ 0 0 "
           "1\n\nNUMVERTS "
           "0\n\nNUMFACES 0\n\nNUMOBJECTS 0\n\nNUMMATERIALS 0\n";
}
} // namespace bm
