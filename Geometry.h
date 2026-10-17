// Brush and patch reconstruction for BrushModeler. No Radiant dependencies, so the tests can build it alone.
#pragma once
#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace bm
{
struct Vec
{
    float x, y, z;
};
inline Vec Sub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec Add(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec Mul(Vec a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec Cross(Vec a, Vec b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float Len(Vec a) { return std::sqrt(Dot(a, a)); }

// One brush face line of .map text: three plane points, material and texture mapping.
struct Tex
{
    Vec p[3];
    std::string material;
    float hs = 64, vs = 64, hshift = 0, vshift = 0, rot = 0;
};
struct Face
{
    std::vector<Vec> p;
    Vec n{};
    std::string material;
    std::vector<std::pair<float, float>> uv;
    std::vector<Vec> normals;
    bool reverseWinding = true;
};
struct PatchGrid
{
    int rows = 0, cols = 0;
    bool curve = false;
    std::string material;
    std::vector<Vec> p;
    std::vector<std::pair<float, float>> uv;
};

// Splits entity text into its brush/patch blocks, keyed by the block's lowercase GUID ("{...}").
std::unordered_map<std::string, std::string> SplitBrushBlocks(const std::string& entityText);
void ParseTex(const std::string& text, std::vector<Tex>& out);
bool ParsePatch(const std::string& text, const char* guid, PatchGrid& out);
bool TessellatePatchCurve(const PatchGrid& g, PatchGrid& out);
bool PatchFaces(const PatchGrid& src, std::vector<Face>& out);
// Rebuilds a convex brush from its planes (9 floats per plane). Faces without a matching texture line get "caulk".
bool Reconstruct(const float mins[3], const float maxs[3], const std::vector<float>& raw, const std::vector<Tex>& tex,
                 std::vector<Face>& out, size_t* untexturedFaces = nullptr);

// Radiant tool materials (caulk, clip, nodraw, portals, triggers, volumes...) that never render.
bool IsToolMaterial(const std::string& material);
// Shifts UVs by whole texture repeats so they stay near 0..1. Large world-space UVs lose precision once compiled.
void RebaseUVs(std::vector<std::pair<float, float>>& uv);

// Shortest fixed-notation text that reads back as the same float.
std::string FormatFloat(float v);

struct ExportStats
{
    size_t vertices = 0, triangles = 0, materials = 0;
};
// XMODEL_EXPORT v6 text for faces placed relative to pivot, all on bone 0 "tag_origin".
std::string BuildXModelExport(const std::vector<Face>& faces, Vec pivot, ExportStats& stats);
// A valid export with no geometry, the base for a models-only merge.
std::string EmptyXModelExport();
} // namespace bm
