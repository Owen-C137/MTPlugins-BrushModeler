// XMODEL_EXPORT v6 parsing, merging and cleanup (a port of the former merge_xmodels.py helper).
// The output matches what that helper wrote, so models merged before and after the port compile the same.
#pragma once
#include <string>
#include <vector>

namespace bm::xm
{
// Each record keeps its whitespace-trimmed source lines; indexed sections are renumbered when merged.
struct Doc
{
    std::vector<std::string> bones;
    std::vector<std::vector<std::string>> boneData, verts, faces;
    std::vector<std::string> objects;
    std::vector<std::vector<std::string>> materials;
};

// Throws std::runtime_error for unsupported or incomplete text. allowEmpty accepts a model with bones only.
Doc Parse(const std::string& text, bool allowEmpty = false);

// Where a source model sits in the map, relative to the combined model's pivot.
struct Placement
{
    double origin[3]{}, center[3]{}, angles[3]{}; // angles: pitch yaw roll in degrees
    double scale = 1;                             // misc_model "modelscale"
};

struct Bounds
{
    bool valid = false;
    double lo[3]{}, hi[3]{};
    void Add(const double p[3]);
};
// "min=(x,y,z) max=(x,y,z)", or "unavailable".
std::string FormatBounds(const Bounds& b);
// Bounds of each vertex's first OFFSET, and of each bone's bind OFFSET.
Bounds VertexOffsetBounds(const Doc& doc);
Bounds BoneOffsetBounds(const Doc& doc);

// The base model renumbered into merge form (the first record of a merge).
Doc Normalized(const Doc& base);

struct AppendReport
{
    const char* strategy = ""; // baked_to_shared_root, baked_static_identity_skeleton or attached_source_skeleton
    size_t sourceBones = 0, sourceVertices = 0, sourceFaces = 0;
    std::string sourceBounds, sourceRoot, transformedBounds, transformedBoneBounds;
};
// Appends a placed model to merged.
AppendReport Append(Doc& merged, const Doc& model, const Placement& placement);

// Reflects vertex positions and normals across the model's local axes (bit 0 X, bit 1 Y, bit 2 Z), reversing the
// winding when an odd number of axes flips. Throws std::runtime_error when there is nothing to mirror.
void Mirror(Doc& doc, unsigned axes, Bounds& source, Bounds& mirrored);

// A GDT skinOverride ("from to" per line) applied to an export's MATERIAL names, as Radiant does for derived models.
struct SkinResult
{
    size_t rules = 0, replaced = 0;
    std::vector<std::string> unmatched; // "from to" rules no material used
};
SkinResult ApplySkinOverrides(std::string& exportText, const std::string& skinOverride);

struct Cleanup
{
    size_t verticesBefore = 0, verticesAfter = 0, facesBefore = 0, facesAfter = 0, degenerateRemoved = 0, duplicateRemoved = 0;
    Bounds bounds; // vertex offsets after cleanup
};
// Validates the bone hierarchy, welds exact duplicate vertices, drops degenerate and exact duplicate faces and
// returns the export text. Throws std::runtime_error on invalid data.
std::string Write(Doc& doc, Cleanup& cleanup);
} // namespace bm::xm
