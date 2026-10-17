// XMODEL_EXPORT v6 parsing, merging and cleanup. Ported line for line from merge_xmodels.py (kept in
// Tests\reference); Tests\RunTests.ps1 checks that both produce identical files.
#include "XModelExport.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace bm::xm
{
namespace
{
using Lines = std::vector<std::string>;

// Python's str.split()/strip() whitespace for ASCII text.
bool IsWs(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || (c >= '\x1c' && c <= '\x1f'); }
std::string Strip(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && IsWs(s[a])) ++a;
    while (b > a && IsWs(s[b - 1])) --b;
    return s.substr(a, b - a);
}
// str.split(maxsplit=n): the remainder after n splits keeps its inner whitespace.
Lines Split(const std::string& s, size_t maxSplit = SIZE_MAX)
{
    Lines out;
    size_t i = 0, n = s.size();
    while (true)
    {
        while (i < n && IsWs(s[i])) ++i;
        if (i >= n) break;
        if (out.size() == maxSplit)
        {
            out.push_back(s.substr(i));
            break;
        }
        size_t j = i;
        while (j < n && !IsWs(s[j])) ++j;
        out.push_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}
std::string Join(const Lines& parts, size_t from)
{
    std::string out;
    for (size_t i = from; i < parts.size(); ++i)
    {
        if (i > from) out += ' ';
        out += parts[i];
    }
    return out;
}
// str.splitlines() for ASCII line breaks.
Lines SplitLines(const std::string& text)
{
    Lines lines;
    size_t start = text.compare(0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0, i = start;
    while (i < text.size())
    {
        char c = text[i];
        if (c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == '\x1c' || c == '\x1d' || c == '\x1e')
        {
            lines.push_back(text.substr(start, i - start));
            i += c == '\r' && i + 1 < text.size() && text[i + 1] == '\n' ? 2 : 1;
            start = i;
        }
        else
            ++i;
    }
    if (start < text.size()) lines.push_back(text.substr(start));
    return lines;
}
bool Starts(const std::string& s, const char* prefix) { return s.compare(0, strlen(prefix), prefix) == 0; }
bool Digits(const std::string& s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}
bool SignedDigits(const std::string& s) { return Digits(s[0] == '-' ? s.substr(1) : s); }
[[noreturn]] void Fail(const std::string& why) { throw std::runtime_error(why); }
long long Int(const std::string& s)
{
    if (s.empty()) Fail("invalid integer ''");
    char* end = nullptr;
    errno = 0;
    long long v = strtoll(s.c_str(), &end, 10);
    if (errno || *end || IsWs(s[0])) Fail("invalid integer '" + s + "'");
    return v;
}
// float(value.strip(","))
double Float(const std::string& raw)
{
    size_t a = raw.find_first_not_of(','), b = raw.find_last_not_of(',');
    std::string s = a == std::string::npos ? std::string() : raw.substr(a, b - a + 1);
    if (s.empty() || IsWs(s[0])) Fail("invalid number '" + raw + "'");
    char* end = nullptr;
    double v = strtod(s.c_str(), &end);
    if (*end) Fail("invalid number '" + raw + "'");
    return v;
}
std::string F6(double v)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%.6f", v);
    return buf;
}

// ^BONE\s+\d+\s+-?\d+\s+  (bone hierarchy line, a name follows)
bool IsBoneHeader(const std::string& s)
{
    Lines t = Split(s, 3);
    return t.size() == 4 && t[0] == "BONE" && Digits(t[1]) && SignedDigits(t[2]);
}
// ^BONE\s+\d+\s*$  (start of a bone's transform record)
bool IsBoneDataStart(const std::string& s)
{
    Lines t = Split(s);
    return t.size() == 2 && t[0] == "BONE" && Digits(t[1]);
}
// ^VERT(?:32)?\s+\d+\s*$
bool IsVertStart(const std::string& s)
{
    Lines t = Split(s);
    return t.size() == 2 && (t[0] == "VERT" || t[0] == "VERT32") && Digits(t[1]);
}

struct Transform
{
    double rot[3][3];
    double origin[3], center[3], scale;
    explicit Transform(const Placement& pl)
        : origin{pl.origin[0], pl.origin[1], pl.origin[2]}, center{pl.center[0], pl.center[1], pl.center[2]}, scale(pl.scale)
    {
        const double degToRad = 3.141592653589793 / 180.0;
        double p = pl.angles[0] * degToRad, y = pl.angles[1] * degToRad, r = pl.angles[2] * degToRad;
        double cp = std::cos(p), sp = std::sin(p), cyaw = std::cos(y), syaw = std::sin(y), cr = std::cos(r), sr = std::sin(r);
        double m[3][3] = {{cyaw * cp, cyaw * sp * sr - syaw * cr, cyaw * sp * cr + syaw * sr},
                          {syaw * cp, syaw * sp * sr + cyaw * cr, syaw * sp * cr - cyaw * sr},
                          {-sp, cp * sr, cp * cr}};
        memcpy(rot, m, sizeof rot);
    }
    // Positions are scaled, rotated and moved; directions only rotated.
    void Apply(double& x, double& y, double& z, bool isOffset) const
    {
        double q[3] = {x, y, z};
        if (isOffset && scale != 1)
            for (double& v : q) v *= scale;
        double out[3];
        for (int i = 0; i < 3; ++i)
        {
            out[i] = rot[i][0] * q[0] + rot[i][1] * q[1] + rot[i][2] * q[2];
            if (isOffset) out[i] = out[i] + origin[i] - center[i];
        }
        x = out[0];
        y = out[1];
        z = out[2];
    }
};

Doc Shifted(const Doc& doc, size_t boneAt, size_t vertAt, size_t objectAt, size_t materialAt, const Transform* transform)
{
    Doc out;
    for (const auto& line : doc.bones)
    {
        Lines t = Split(line, 3);
        long long idx = Int(t[1]) + (long long)boneAt, parent = Int(t[2]);
        // XMODEL_EXPORT supports one hierarchy root. Attach each appended model's
        // former root to the combined tag_origin instead of emitting another -1 parent.
        long long mergedParent = parent >= 0 ? parent + (long long)boneAt : (boneAt > 0 ? 0 : -1);
        std::string boneName = t[3];
        if (boneAt > 0 && transform)
        {
            // Repeated instances often share identical skeleton bone names. Keep each appended instance's
            // skeleton independent when export2bin builds the combined model, so one instance's bind pose
            // cannot override another.
            std::string suffix = "__bm" + std::to_string(boneAt);
            size_t keep = suffix.size() < 63 ? 63 - suffix.size() : 0;
            std::string raw = boneName.size() >= 1 && boneName.front() == '"' && boneName.back() == '"'
                                  ? (boneName.size() >= 2 ? boneName.substr(1, boneName.size() - 2) : std::string())
                                  : boneName;
            boneName = "\"" + raw.substr(0, keep) + suffix + "\"";
        }
        out.bones.push_back("BONE " + std::to_string(idx) + " " + std::to_string(mergedParent) + " " + boneName);
    }
    for (const auto& chunk : doc.boneData)
    {
        Lines o;
        for (const auto& line : chunk)
        {
            if (Starts(line, "BONE "))
                o.push_back("BONE " + std::to_string(Int(Split(line)[1]) + (long long)boneAt));
            else if ((Starts(line, "OFFSET ") || Starts(line, "X ") || Starts(line, "Y ") || Starts(line, "Z ")) && transform)
            {
                // ExportX stores each joint's bind-pose transform in model space. Transform every joint,
                // not just tag_origin, or a rigged mesh's weighted vertices remain around its unplaced skeleton.
                // ExportX writes "x, y, z" while BrushModeler writes whitespace-separated values; accept both.
                Lines t = Split(line);
                if (t.size() < 4) Fail("bone vector needs three values: " + line);
                double x = Float(t[1]), y = Float(t[2]), z = Float(t[3]);
                transform->Apply(x, y, z, t[0] == "OFFSET");
                o.push_back(t[0] + " " + F6(x) + " " + F6(y) + " " + F6(z));
            }
            else
                o.push_back(line);
        }
        out.boneData.push_back(std::move(o));
    }
    for (const auto& chunk : doc.verts)
    {
        Lines o;
        for (const auto& line : chunk)
        {
            if (Starts(line, "VERT"))
            {
                Lines t = Split(line);
                if (t.size() < 2) Fail("invalid vertex line: " + line);
                o.push_back(t[0] + " " + std::to_string(Int(t[1]) + (long long)vertAt));
            }
            else if (Starts(line, "BONE "))
            {
                Lines t = Split(line);
                if (t.size() < 2) Fail("invalid vertex weight: " + line);
                o.push_back("BONE " + std::to_string(Int(t[1]) + (long long)boneAt) + " " + Join(t, 2));
            }
            else
                o.push_back(line);
        }
        out.verts.push_back(std::move(o));
    }
    for (const auto& chunk : doc.faces)
    {
        Lines o;
        for (const auto& line : chunk)
        {
            if (Starts(line, "TRI "))
            {
                Lines t = Split(line);
                if (t.size() < 3) Fail("invalid triangle: " + line);
                o.push_back("TRI " + std::to_string(Int(t[1]) + (long long)objectAt) + " " +
                            std::to_string(Int(t[2]) + (long long)materialAt) + " " + Join(t, 3));
            }
            else if (Starts(line, "VERT"))
            {
                Lines t = Split(line);
                if (t.size() < 2) Fail("invalid face corner: " + line);
                o.push_back(t[0] + " " + std::to_string(Int(t[1]) + (long long)vertAt));
            }
            else
                o.push_back(line);
        }
        out.faces.push_back(std::move(o));
    }
    for (const auto& line : doc.objects)
    {
        Lines t = Split(line, 2);
        if (t.size() < 3) Fail("invalid object: " + line);
        out.objects.push_back("OBJECT " + std::to_string(Int(t[1]) + (long long)objectAt) + " " + t[2]);
    }
    for (const auto& chunk : doc.materials)
    {
        Lines o = chunk;
        Lines t = Split(o[0], 2);
        if (t.size() < 3) Fail("invalid material: " + o[0]);
        o[0] = "MATERIAL " + std::to_string(Int(t[1]) + (long long)materialAt) + " " + t[2];
        out.materials.push_back(std::move(o));
    }
    return out;
}

void AppendBits(std::string& key, double v)
{
    if (v == 0) v = 0; // -0 and 0 weld, as they compare equal
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    char buf[20];
    snprintf(buf, sizeof buf, "%016llx", (unsigned long long)bits);
    key += buf;
}

// A conservative exact key; face-corner UVs/normals stay on faces.
std::string VertexKey(const Lines& chunk, long long fallbackIndex, size_t& nanCounter)
{
    bool hasPosition = false, hasBoneCount = false, hasNan = false;
    double position[3]{};
    long long boneCount = 0;
    std::vector<std::pair<long long, std::vector<double>>> weights;
    std::vector<Lines> extra;
    for (size_t i = 1; i < chunk.size(); ++i)
    {
        Lines parts = Split(chunk[i]);
        if (parts.empty()) continue;
        if (parts[0] == "OFFSET" && parts.size() >= 4)
        {
            for (int k = 0; k < 3; ++k) position[k] = Float(parts[1 + k]);
            hasPosition = true;
        }
        else if (parts[0] == "BONES" && parts.size() >= 2)
        {
            boneCount = Int(parts[1]);
            hasBoneCount = true;
        }
        else if (parts[0] == "BONE" && parts.size() >= 3)
        {
            std::vector<double> values;
            for (size_t k = 2; k < parts.size(); ++k) values.push_back(Float(parts[k]));
            weights.emplace_back(Int(parts[1]), std::move(values));
        }
        else
            // Unknown per-vertex attributes must match exactly before welding.
            extra.push_back(std::move(parts));
    }
    // A vertex without a recognized position cannot safely be merged.
    if (!hasPosition) return "U" + std::to_string(fallbackIndex);
    std::sort(weights.begin(), weights.end());
    std::string key = "P";
    for (double v : position)
    {
        hasNan |= std::isnan(v);
        AppendBits(key, v);
    }
    key += hasBoneCount ? "|B" + std::to_string(boneCount) : std::string("|-");
    for (const auto& w : weights)
    {
        key += "|W" + std::to_string(w.first) + ":" + std::to_string(w.second.size()) + ":";
        for (double v : w.second)
        {
            hasNan |= std::isnan(v);
            AppendBits(key, v);
        }
    }
    for (const auto& parts : extra)
    {
        key += "|X" + std::to_string(parts.size());
        for (const auto& p : parts) key += ":" + std::to_string(p.size()) + ":" + p;
    }
    // NaN never equals itself, so such a vertex is never welded.
    if (hasNan) return "N" + std::to_string(nanCounter++);
    return key;
}

std::string FaceKey(const Lines& face)
{
    std::string key;
    for (const auto& line : face) key += std::to_string(line.size()) + ":" + line;
    return key;
}

// Weld exact duplicate vertices and discard only degenerate/exact duplicate faces.
Cleanup OptimizeMesh(Doc& d)
{
    Cleanup c;
    c.verticesBefore = d.verts.size();
    c.facesBefore = d.faces.size();
    std::unordered_map<long long, long long> remap;
    std::vector<Lines> unique;
    std::unordered_map<std::string, long long> byKey;
    size_t nanCounter = 0;
    for (const auto& chunk : d.verts)
    {
        Lines header = Split(chunk[0]);
        long long oldIndex = Int(header[1]);
        std::string key = VertexKey(chunk, oldIndex, nanCounter);
        auto found = byKey.find(key);
        long long newIndex;
        if (found == byKey.end())
        {
            newIndex = (long long)unique.size();
            byKey.emplace(std::move(key), newIndex);
            Lines u;
            u.push_back(std::string(header[0] == "VERT32" ? "VERT32" : "VERT") + " " + std::to_string(newIndex));
            u.insert(u.end(), chunk.begin() + 1, chunk.end());
            unique.push_back(std::move(u));
        }
        else
            newIndex = found->second;
        remap[oldIndex] = newIndex;
    }

    std::vector<Lines> kept;
    std::unordered_set<std::string> faceKeys;
    for (const auto& chunk : d.faces)
    {
        Lines out;
        std::vector<long long> corners;
        for (const auto& line : chunk)
        {
            if (Starts(line, "VERT ") || Starts(line, "VERT32 "))
            {
                Lines parts = Split(line);
                long long oldIndex = Int(parts[1]);
                auto it = remap.find(oldIndex);
                if (it == remap.end()) Fail("face references missing vertex " + std::to_string(oldIndex));
                out.push_back(parts[0] + " " + std::to_string(it->second));
                corners.push_back(it->second);
            }
            else
                out.push_back(line);
        }
        if (corners.size() >= 3 && (corners[0] == corners[1] || corners[1] == corners[2] || corners[0] == corners[2]))
        {
            ++c.degenerateRemoved;
            continue;
        }
        // Preserve winding, material, normals, colors and UVs; only exact repeated
        // face records are removed. No geometric simplification is performed.
        if (!faceKeys.insert(FaceKey(out)).second)
        {
            ++c.duplicateRemoved;
            continue;
        }
        kept.push_back(std::move(out));
    }
    d.verts = std::move(unique);
    d.faces = std::move(kept);
    c.verticesAfter = d.verts.size();
    c.facesAfter = d.faces.size();
    return c;
}

// True when a model's vertices all use its one root bone.
bool SingleRoot(const Doc& doc)
{
    if (doc.bones.size() != 1 || doc.boneData.size() != 1) return false;
    bool any = false;
    for (const auto& chunk : doc.verts)
        for (const auto& line : chunk)
            if (Starts(line, "BONE "))
            {
                if (Int(Split(line)[1]) != 0) return false;
                any = true;
            }
    return any;
}

// Multi-bone exports that are really static mesh groups: every bone has an identity bind transform and every
// vertex one full-weight bone, so their placement can be baked into vertices without changing the visible mesh.
bool StaticIdentitySkeleton(const Doc& doc)
{
    if (doc.bones.size() <= 1 || doc.boneData.size() != doc.bones.size()) return false;
    static const char* keys[] = {"OFFSET", "X", "Y", "Z"};
    static const double expected[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    try
    {
        for (const auto& chunk : doc.boneData)
        {
            bool seen[4] = {};
            double values[4][3]{};
            size_t counts[4] = {};
            for (const auto& line : chunk)
            {
                Lines parts = Split(line);
                if (parts.empty()) continue;
                for (int k = 0; k < 4; ++k)
                    if (parts[0] == keys[k])
                    {
                        seen[k] = true;
                        counts[k] = (std::min)(parts.size() - 1, (size_t)3);
                        for (size_t v = 0; v < counts[k]; ++v) values[k][v] = Float(parts[1 + v]);
                    }
            }
            for (int k = 0; k < 4; ++k)
            {
                if (!seen[k] || counts[k] != 3) return false;
                for (int v = 0; v < 3; ++v)
                    if (std::fabs(values[k][v] - expected[k][v]) > 1e-6) return false;
            }
        }
        for (const auto& chunk : doc.verts)
        {
            bool hasCount = false;
            long long boneCount = 0;
            std::vector<std::pair<long long, double>> weights;
            for (size_t i = 1; i < chunk.size(); ++i)
            {
                Lines parts = Split(chunk[i]);
                if (parts.empty()) continue;
                if (parts[0] == "BONES" && parts.size() >= 2)
                {
                    boneCount = Int(parts[1]);
                    hasCount = true;
                }
                else if (parts[0] == "BONE" && parts.size() >= 3)
                    weights.emplace_back(Int(parts[1]), Float(parts[2]));
            }
            if (!hasCount || boneCount != 1 || weights.size() != 1 || weights[0].first < 0 ||
                weights[0].first >= (long long)doc.bones.size() || std::fabs(weights[0].second - 1.0) > 1e-6)
                return false;
        }
    }
    catch (const std::runtime_error&)
    {
        return false;
    }
    return !doc.verts.empty();
}

struct RootVectors
{
    double offset[3] = {0, 0, 0}, scale[3] = {1, 1, 1}, axes[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
};
RootVectors ReadRootVectors(const Doc& doc)
{
    RootVectors r;
    for (const auto& line : doc.boneData[0])
    {
        Lines parts = Split(line);
        if (parts.empty()) continue;
        double* target = parts[0] == "OFFSET"  ? r.offset
                         : parts[0] == "SCALE" ? r.scale
                         : parts[0] == "X"     ? r.axes[0]
                         : parts[0] == "Y"     ? r.axes[1]
                         : parts[0] == "Z"     ? r.axes[2]
                                               : nullptr;
        if (!target) continue;
        if (parts.size() < 4) Fail("root bone vector needs three values: " + line);
        for (int k = 0; k < 3; ++k) target[k] = Float(parts[1 + k]);
    }
    return r;
}

// Bake a static one-root model's skeleton and placement into vertices and reuse bone 0.
Doc ShiftedBakedSingleRoot(const Doc& doc, size_t vertAt, size_t objectAt, size_t materialAt, const Transform& transform)
{
    Doc out = Shifted(doc, 0, vertAt, objectAt, materialAt, nullptr);
    out.bones.clear();
    out.boneData.clear();
    RootVectors root = ReadRootVectors(doc);
    const double* offset = root.offset;
    const double* scale = root.scale;
    auto bakePosition = [&](double& x, double& y, double& z) {
        double scaled[3] = {x * scale[0], y * scale[1], z * scale[2]}, local[3];
        for (int i = 0; i < 3; ++i)
            local[i] = offset[i] + (root.axes[0][i] * scaled[0] + root.axes[1][i] * scaled[1] + root.axes[2][i] * scaled[2]);
        x = local[0];
        y = local[1];
        z = local[2];
        transform.Apply(x, y, z, true);
    };
    auto bakeNormal = [&](double& x, double& y, double& z) {
        double scaled[3] = {scale[0] ? x / scale[0] : 0.0, scale[1] ? y / scale[1] : 0.0, scale[2] ? z / scale[2] : 0.0}, local[3];
        for (int i = 0; i < 3; ++i) local[i] = root.axes[0][i] * scaled[0] + root.axes[1][i] * scaled[1] + root.axes[2][i] * scaled[2];
        x = local[0];
        y = local[1];
        z = local[2];
        transform.Apply(x, y, z, false);
        double length = std::sqrt(x * x + y * y + z * z);
        if (length)
        {
            x /= length;
            y /= length;
            z /= length;
        }
    };
    // A negative determinant mirrors the root coordinate system. Reverse each triangle's corner records
    // in that case so its winding remains consistent with the inverse-transpose normal transform.
    const double (*a)[3] = root.axes;
    double determinant = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[1][0] * (a[0][1] * a[2][2] - a[0][2] * a[2][1]) +
                         a[2][0] * (a[0][1] * a[1][2] - a[0][2] * a[1][1]);
    determinant *= scale[0] * scale[1] * scale[2];

    for (auto& chunk : out.verts)
        for (auto& line : chunk)
        {
            if (Starts(line, "OFFSET "))
            {
                Lines t = Split(line);
                if (t.size() < 4) Fail("vertex offset needs three values: " + line);
                double x = Float(t[1]), y = Float(t[2]), z = Float(t[3]);
                bakePosition(x, y, z);
                line = "OFFSET " + F6(x) + " " + F6(y) + " " + F6(z);
            }
            else if (Starts(line, "BONE "))
            {
                Lines t = Split(line, 2);
                line = "BONE 0 " + (t.size() > 2 ? t[2] : std::string("1"));
            }
        }
    for (auto& chunk : out.faces)
    {
        for (auto& line : chunk)
            if (Starts(line, "NORMAL "))
            {
                Lines t = Split(line);
                if (t.size() < 4) Fail("normal needs three values: " + line);
                double x = Float(t[1]), y = Float(t[2]), z = Float(t[3]);
                bakeNormal(x, y, z);
                line = "NORMAL " + F6(x) + " " + F6(y) + " " + F6(z);
            }
        if (determinant < 0.0)
        {
            // Keep TRI metadata intact while swapping the second and third full corner records together.
            Lines prefix;
            std::vector<Lines> corners;
            for (const auto& line : chunk)
            {
                if (Starts(line, "VERT ") || Starts(line, "VERT32 "))
                    corners.push_back({line});
                else if (corners.empty())
                    prefix.push_back(line);
                else
                    corners.back().push_back(line);
            }
            if (corners.size() == 3)
            {
                Lines swapped = prefix;
                for (int k : {0, 2, 1}) swapped.insert(swapped.end(), corners[k].begin(), corners[k].end());
                chunk = std::move(swapped);
            }
        }
    }
    return out;
}

template <typename T> void Concat(std::vector<T>& a, std::vector<T>&& b)
{
    a.insert(a.end(), std::make_move_iterator(b.begin()), std::make_move_iterator(b.end()));
}

// First OFFSET of each record, the way the audit log has always measured bounds.
Bounds FirstOffsetBounds(const std::vector<Lines>& records)
{
    Bounds b;
    for (const auto& chunk : records)
        for (const auto& line : chunk)
            if (Starts(line, "OFFSET "))
            {
                Lines t = Split(line);
                if (t.size() >= 4)
                {
                    double p[3] = {Float(t[1]), Float(t[2]), Float(t[3])};
                    b.Add(p);
                }
                break;
            }
    return b;
}

std::string FormatRoot(const Doc& doc)
{
    if (doc.boneData.empty()) return "unavailable";
    RootVectors r;
    try
    {
        r = ReadRootVectors(doc);
    }
    catch (const std::runtime_error&)
    {
        return "unreadable";
    }
    auto vec = [](const double* v) {
        char buf[96];
        snprintf(buf, sizeof buf, "%.4f,%.4f,%.4f", v[0], v[1], v[2]);
        return std::string(buf);
    };
    return "OFFSET=" + vec(r.offset) + " SCALE=" + vec(r.scale) + " X=" + vec(r.axes[0]) + " Y=" + vec(r.axes[1]) + " Z=" + vec(r.axes[2]);
}

// "-x" for "x" and back, keeping ExportX's trailing comma and every digit.
std::string NegateToken(const std::string& token)
{
    size_t end = token.find_last_not_of(',');
    std::string core = end == std::string::npos ? std::string() : token.substr(0, end + 1), tail = token.substr(core.size());
    if (core.empty()) Fail("invalid number '" + token + "'");
    if (core[0] == '-')
        core.erase(0, 1);
    else if (core[0] == '+')
        core[0] = '-';
    else
        core.insert(0, 1, '-');
    return core + tail;
}
} // namespace

void Bounds::Add(const double p[3])
{
    for (int k = 0; k < 3; ++k)
    {
        lo[k] = valid ? (std::min)(lo[k], p[k]) : p[k];
        hi[k] = valid ? (std::max)(hi[k], p[k]) : p[k];
    }
    valid = true;
}
std::string FormatBounds(const Bounds& b)
{
    if (!b.valid) return "unavailable";
    char buf[256];
    snprintf(buf, sizeof buf, "min=(%.3f,%.3f,%.3f) max=(%.3f,%.3f,%.3f)", b.lo[0], b.lo[1], b.lo[2], b.hi[0], b.hi[1], b.hi[2]);
    return buf;
}
Bounds VertexOffsetBounds(const Doc& doc) { return FirstOffsetBounds(doc.verts); }
Bounds BoneOffsetBounds(const Doc& doc) { return FirstOffsetBounds(doc.boneData); }

void Mirror(Doc& doc, unsigned axes, Bounds& source, Bounds& mirrored)
{
    if (!(axes & 7)) Fail("choose at least one mirror axis");
    if (doc.verts.empty() || doc.faces.empty()) Fail("the source model contains no vertices or faces to mirror");
    source = mirrored = Bounds{};
    auto reflect = [&](std::string& line, bool position) {
        Lines t = Split(line);
        if (t.size() < 4) Fail("vector needs three values: " + line);
        double original[3], reflected[3];
        for (int k = 0; k < 3; ++k)
        {
            original[k] = Float(t[1 + k]);
            bool flip = (axes >> k) & 1;
            reflected[k] = flip ? -original[k] : original[k];
            if (flip) t[1 + k] = NegateToken(t[1 + k]);
        }
        if (position)
        {
            source.Add(original);
            mirrored.Add(reflected);
        }
        line = Join(t, 0);
    };
    for (auto& chunk : doc.verts)
        for (auto& line : chunk)
            if (Starts(line, "OFFSET ")) reflect(line, true);
    bool reverse = (((axes & 1) != 0) ^ ((axes & 2) != 0) ^ ((axes & 4) != 0));
    for (auto& chunk : doc.faces)
    {
        for (auto& line : chunk)
            if (Starts(line, "NORMAL ")) reflect(line, false);
        if (!reverse) continue;
        // Swap the second and third full corner records; blank lines after the last corner stay at the end.
        Lines prefix, trailing;
        std::vector<Lines> corners;
        Lines body = chunk;
        while (!body.empty() && body.back().empty())
        {
            trailing.push_back(body.back());
            body.pop_back();
        }
        for (const auto& line : body)
        {
            if (Starts(line, "VERT ") || Starts(line, "VERT32 "))
                corners.push_back({line});
            else if (corners.empty())
                prefix.push_back(line);
            else
                corners.back().push_back(line);
        }
        if (corners.size() != 3) Fail("could not safely reverse a mirrored triangle");
        Lines swapped = prefix;
        for (int k : {0, 2, 1}) swapped.insert(swapped.end(), corners[k].begin(), corners[k].end());
        swapped.insert(swapped.end(), trailing.rbegin(), trailing.rend());
        chunk = std::move(swapped);
    }
}

SkinResult ApplySkinOverrides(std::string& text, const std::string& skinOverride)
{
    SkinResult result;
    // Rules come decoded from the GDT ("\r\n" as line breaks); older callers passed the escapes literally.
    std::string normalized = skinOverride;
    for (const char* escape : {"\\r\\n", "\\n"})
        for (size_t p = 0; (p = normalized.find(escape, p)) != std::string::npos;) normalized.replace(p, strlen(escape), "\n");
    std::vector<std::pair<std::string, std::string>> rules;
    for (const auto& line : SplitLines(normalized))
    {
        Lines t = Split(line);
        if (t.size() >= 2) rules.emplace_back(t[0], t[1]);
    }
    result.rules = rules.size();
    if (rules.empty()) return result;
    std::vector<size_t> matches(rules.size());
    std::string out;
    out.reserve(text.size());
    size_t pos = 0;
    while (pos < text.size())
    {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() : nl + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // ^(MATERIAL\s+\d+\s+")([^"]+)(".*)$
        Lines head = Split(line, 2);
        if (Starts(line, "MATERIAL") && head.size() == 3 && head[0] == "MATERIAL" && Digits(head[1]) && !head[2].empty() &&
            head[2][0] == '"')
        {
            size_t open = line.size() - head[2].size(), close = line.find('"', open + 1);
            if (close != std::string::npos && close > open + 1)
            {
                std::string material = line.substr(open + 1, close - open - 1);
                for (size_t i = 0; i < rules.size(); ++i)
                    if (material == rules[i].first)
                    {
                        material = rules[i].second;
                        ++matches[i];
                    }
                line = line.substr(0, open + 1) + material + line.substr(close);
            }
        }
        out += line;
        out += '\n';
    }
    text = std::move(out);
    for (size_t i = 0; i < rules.size(); ++i)
    {
        result.replaced += matches[i];
        if (!matches[i]) result.unmatched.push_back(rules[i].first + " " + rules[i].second);
    }
    return result;
}

Doc Parse(const std::string& text, bool allowEmpty)
{
    Lines lines = SplitLines(text);
    for (auto& line : lines) line = Strip(line);
    const size_t n = lines.size();
    Doc d;
    size_t i = 0;
    while (i < n)
    {
        Lines t = Split(lines[i]);
        if (t.size() == 2 && t[0] == "NUMBONES")
        {
            long long count = Int(t[1]);
            ++i;
            while (i < n && (long long)d.bones.size() < count)
            {
                if (IsBoneHeader(lines[i])) d.bones.push_back(lines[i]);
                ++i;
            }
            while (i < n && (long long)d.boneData.size() < count)
            {
                if (IsBoneDataStart(lines[i]))
                {
                    Lines chunk{lines[i++]};
                    while (i < n && !lines[i].empty() && !IsBoneDataStart(lines[i])) chunk.push_back(lines[i++]);
                    d.boneData.push_back(std::move(chunk));
                    continue;
                }
                ++i;
            }
            continue;
        }
        if (t.size() == 2 && (t[0] == "NUMVERTS" || t[0] == "NUMVERTS32"))
        {
            long long count = Int(t[1]);
            ++i;
            while (i < n && (long long)d.verts.size() < count)
            {
                if (IsVertStart(lines[i]))
                {
                    Lines chunk{lines[i++]};
                    while (i < n && !lines[i].empty() && !IsVertStart(lines[i]) && !Starts(lines[i], "NUMFACES") &&
                           !Starts(lines[i], "NUMOBJECTS") && !Starts(lines[i], "NUMMATERIALS"))
                        chunk.push_back(lines[i++]);
                    d.verts.push_back(std::move(chunk));
                    continue;
                }
                ++i;
            }
            continue;
        }
        if (t.size() == 2 && t[0] == "NUMFACES")
        {
            long long count = Int(t[1]);
            ++i;
            while (i < n && (long long)d.faces.size() < count)
            {
                if (Starts(lines[i], "TRI "))
                {
                    Lines chunk{lines[i++]};
                    while (i < n && !Starts(lines[i], "TRI ") && !Starts(lines[i], "NUMOBJECTS") && !Starts(lines[i], "NUMMATERIALS"))
                        chunk.push_back(lines[i++]);
                    d.faces.push_back(std::move(chunk));
                    continue;
                }
                ++i;
            }
            continue;
        }
        if (t.size() == 2 && t[0] == "NUMOBJECTS")
        {
            long long count = Int(t[1]);
            ++i;
            while (i < n && (long long)d.objects.size() < count)
            {
                if (Starts(lines[i], "OBJECT ")) d.objects.push_back(lines[i]);
                ++i;
            }
            continue;
        }
        if (t.size() == 2 && t[0] == "NUMMATERIALS")
        {
            long long count = Int(t[1]);
            ++i;
            while (i < n && (long long)d.materials.size() < count)
            {
                if (Starts(lines[i], "MATERIAL "))
                {
                    Lines chunk{lines[i++]};
                    while (i < n && !lines[i].empty() && !Starts(lines[i], "MATERIAL ")) chunk.push_back(lines[i++]);
                    d.materials.push_back(std::move(chunk));
                    continue;
                }
                ++i;
            }
            continue;
        }
        ++i;
    }
    if (d.bones.empty() || (!allowEmpty && (d.verts.empty() || d.faces.empty() || d.objects.empty() || d.materials.empty())))
        Fail("unsupported or incomplete XMODEL_EXPORT sections");
    return d;
}

Doc Normalized(const Doc& base) { return Shifted(base, 0, 0, 0, 0, nullptr); }

AppendReport Append(Doc& merged, const Doc& model, const Placement& placement)
{
    Transform transform(placement);
    AppendReport report;
    report.sourceBones = model.bones.size();
    report.sourceVertices = model.verts.size();
    report.sourceFaces = model.faces.size();
    report.sourceBounds = FormatBounds(VertexOffsetBounds(model));
    report.sourceRoot = FormatRoot(model);
    Doc shifted;
    if (SingleRoot(model))
    {
        // A second tag_origin bone's offset is not applied consistently by Radiant after export2bin. Bake the
        // entity-relative position into the static model vertices and bind them to the brush model's existing root.
        shifted = ShiftedBakedSingleRoot(model, merged.verts.size(), merged.objects.size(), merged.materials.size(), transform);
        report.strategy = "baked_to_shared_root";
    }
    else if (StaticIdentitySkeleton(model))
    {
        // Some static props split mesh groups across independent identity bones. export2bin/Radiant does not
        // reliably preserve per-instance placement for these appended skeletons, so bake each into the shared root.
        shifted = ShiftedBakedSingleRoot(model, merged.verts.size(), merged.objects.size(), merged.materials.size(), transform);
        report.strategy = "baked_static_identity_skeleton";
    }
    else
    {
        shifted = Shifted(model, merged.bones.size(), merged.verts.size(), merged.objects.size(), merged.materials.size(), &transform);
        report.strategy = "attached_source_skeleton";
    }
    report.transformedBounds = FormatBounds(VertexOffsetBounds(shifted));
    report.transformedBoneBounds = FormatBounds(BoneOffsetBounds(shifted));
    Concat(merged.bones, std::move(shifted.bones));
    Concat(merged.boneData, std::move(shifted.boneData));
    Concat(merged.verts, std::move(shifted.verts));
    Concat(merged.faces, std::move(shifted.faces));
    Concat(merged.objects, std::move(shifted.objects));
    Concat(merged.materials, std::move(shifted.materials));
    return report;
}

std::string Write(Doc& d, Cleanup& cleanup)
{
    for (size_t expected = 0; expected < d.bones.size(); ++expected)
    {
        const std::string& line = d.bones[expected];
        if (!IsBoneHeader(line)) Fail("invalid bone index at slot " + std::to_string(expected) + ": " + line);
        Lines t = Split(line, 3);
        long long index = Int(t[1]), parent = Int(t[2]);
        if (index != (long long)expected) Fail("invalid bone index at slot " + std::to_string(expected) + ": " + line);
        if ((expected == 0 && parent != -1) || (expected > 0 && !(0 <= parent && parent < (long long)expected)))
            Fail("invalid bone parent at bone " + std::to_string(expected) + ": parent=" + std::to_string(parent));
    }
    cleanup = OptimizeMesh(d);
    cleanup.bounds = VertexOffsetBounds(d);
    bool wide = std::any_of(d.verts.begin(), d.verts.end(), [](const Lines& c) { return Starts(c[0], "VERT32 "); });
    std::string o;
    auto line = [&](const std::string& s) {
        o += s;
        o += '\n';
    };
    auto vertLine = [&](const std::string& s) {
        if (wide && Starts(s, "VERT "))
            line("VERT32 " + s.substr(5));
        else
            line(s);
    };
    line("MODEL");
    line("VERSION 6");
    line("");
    line("NUMBONES " + std::to_string(d.bones.size()));
    for (const auto& b : d.bones) line(b);
    line("");
    for (const auto& c : d.boneData)
        for (const auto& l : c) line(l);
    line("");
    line(std::string(wide ? "NUMVERTS32 " : "NUMVERTS ") + std::to_string(d.verts.size()));
    line("");
    for (const auto& c : d.verts)
        for (const auto& l : c) vertLine(l);
    line("");
    line("NUMFACES " + std::to_string(d.faces.size()));
    line("");
    for (const auto& c : d.faces)
        for (const auto& l : c) vertLine(l);
    line("");
    line("NUMOBJECTS " + std::to_string(d.objects.size()));
    for (const auto& obj : d.objects) line(obj);
    line("");
    line("NUMMATERIALS " + std::to_string(d.materials.size()));
    for (const auto& c : d.materials)
        for (const auto& l : c) line(l);
    return o;
}
} // namespace bm::xm
