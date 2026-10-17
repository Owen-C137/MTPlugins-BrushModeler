// BrushModeler tests: unit checks of the geometry, GDT and XMODEL_EXPORT code, plus command-line entry points that
// RunTests.ps1 uses to compare the C++ merge against the reference Python helper on real ExportX output.
#include "../Gdt.h"
#include "../Geometry.h"
#include "../XModelExport.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace bm;

namespace
{
int g_failures = 0, g_checks = 0;
#define CHECK(cond)                                                                                                                        \
    do                                                                                                                                     \
    {                                                                                                                                      \
        ++g_checks;                                                                                                                        \
        if (!(cond))                                                                                                                       \
        {                                                                                                                                  \
            ++g_failures;                                                                                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                                    \
        }                                                                                                                                  \
    } while (0)

std::string ReadAll(const char* path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error(std::string("cannot read ") + path);
    return std::string((std::istreambuf_iterator<char>(in)), {});
}
void WriteAll(const char* path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) throw std::runtime_error(std::string("cannot write ") + path);
}
size_t Count(const std::string& text, const std::string& needle)
{
    size_t n = 0;
    for (size_t p = text.find(needle); p != std::string::npos; p = text.find(needle, p + 1)) ++n;
    return n;
}

// Two brushes and a curve patch in one entity, as WriteEntityTextWith gives them. The brushes share their first
// plane's points but not its material, which the old entity-wide texture lookup mixed up.
const char* kEntityText = R"(// entity 0
{
guid "{5CCAB01E-114C-11E5-939F-2C27D7EF3DD8}"
"classname" "worldspawn"
// brush 0
{
 guid "{AAAAAAAA-0000-0000-0000-000000000001}"
 ( 0 0 64 ) ( 64 0 64 ) ( 0 64 64 ) top_a 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 0 0 ) ( 0 64 0 ) ( 64 0 0 ) bottom_a 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 0 0 ) ( 0 0 64 ) ( 0 64 0 ) caulk 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 64 0 0 ) ( 64 64 0 ) ( 64 0 64 ) side_a 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 0 0 ) ( 64 0 0 ) ( 0 0 64 ) side_a 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 64 0 ) ( 0 64 64 ) ( 64 64 0 ) side_a 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
}
// brush 1
{
 guid "{BBBBBBBB-0000-0000-0000-000000000002}"
 ( 0 0 64 ) ( 64 0 64 ) ( 0 64 64 ) top_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 0 32 ) ( 0 64 32 ) ( 64 0 32 ) bottom_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 0 0 ) ( 0 0 64 ) ( 0 64 0 ) side_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 64 0 0 ) ( 64 64 0 ) ( 64 0 64 ) side_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 0 0 ) ( 64 0 0 ) ( 0 0 64 ) side_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
 ( 0 64 0 ) ( 0 64 64 ) ( 64 64 0 ) side_b 64 64 0 0 0 0 lightmap_gray 16384 16384 0 0 0 0
}
// brush 2
 {
  guid "{CCCCCCCC-0000-0000-0000-000000000003}"
  curve
  {
  toolFlags;
   t7_metal_worn_iron_dark
   lightmap_gray
   3 3 16 1
   (
	v 0 0 0 t 0 0 1 1
	v 32 0 16 t 512 0 1 1
	v 64 0 0 t 1024 0 1 1
   )
   (
	v 0 32 0 t 0 512 1 1
	v 32 32 16 t 512 512 1 1
	v 64 32 0 t 1024 512 1 1
   )
   (
	v 0 64 0 t 0 1024 1 1
	v 32 64 16 t 512 1024 1 1
	v 64 64 0 t 1024 1024 1 1
   )
  }
 }
}
)";

std::vector<float> PlanesOf(const std::vector<Tex>& tex)
{
    std::vector<float> raw;
    for (const auto& t : tex)
        for (const auto& p : t.p)
        {
            raw.push_back(p.x);
            raw.push_back(p.y);
            raw.push_back(p.z);
        }
    return raw;
}

void TestBrushBlocks()
{
    auto blocks = SplitBrushBlocks(kEntityText);
    CHECK(blocks.size() == 3);
    CHECK(blocks.count("{aaaaaaaa-0000-0000-0000-000000000001}") == 1);
    const std::string& a = blocks["{aaaaaaaa-0000-0000-0000-000000000001}"];
    const std::string& b = blocks["{bbbbbbbb-0000-0000-0000-000000000002}"];
    std::vector<Tex> ta, tb;
    ParseTex(a, ta);
    ParseTex(b, tb);
    CHECK(ta.size() == 6 && tb.size() == 6);

    // Brush B rebuilt from its own block keeps its own top material although A has the same plane points.
    float minsB[3] = {0, 0, 32}, maxsB[3] = {64, 64, 64};
    std::vector<Face> faces;
    size_t missing = 0;
    CHECK(Reconstruct(minsB, maxsB, PlanesOf(tb), tb, faces, &missing));
    CHECK(faces.size() == 6 && missing == 0);
    bool topB = false, topA = false;
    for (const auto& f : faces)
    {
        topB |= f.material == "top_b";
        topA |= f.material == "top_a";
        CHECK(f.p.size() == 4);
    }
    CHECK(topB && !topA);

    // Plane points that differ from the text's still find the texture through the plane equation.
    std::vector<float> shifted = PlanesOf(ta);
    for (size_t i = 0; i < 9; i += 3) shifted[i] += 8; // slide the top plane's points along X
    float minsA[3] = {0, 0, 0}, maxsA[3] = {64, 64, 64};
    std::vector<Face> facesA;
    missing = 0;
    CHECK(Reconstruct(minsA, maxsA, shifted, ta, facesA, &missing));
    CHECK(missing == 0);

    PatchGrid patch;
    CHECK(!ParsePatch(a, "{AAAAAAAA-0000-0000-0000-000000000001}", patch));
    CHECK(ParsePatch(blocks["{cccccccc-0000-0000-0000-000000000003}"], "{CCCCCCCC-0000-0000-0000-000000000003}", patch));
    CHECK(patch.curve && patch.rows == 3 && patch.cols == 3 && patch.material == "t7_metal_worn_iron_dark");
    std::vector<Face> patchFaces;
    CHECK(PatchFaces(patch, patchFaces));
    CHECK(patchFaces.size() >= 8);
}

void TestMaterialsAndUVs()
{
    for (const char* tool :
         {"caulk", "clip", "clip_player", "nodraw_decal", "portal_nodraw", "sky", "trigger_hint", "umbra_volume", "hint", "ladder_metal"})
        CHECK(IsToolMaterial(tool));
    for (const char* real :
         {"sky_signage_letters_dlc1", "clipboard_paper", "t7_metal_worn_iron_dark", "caulking_gun", "portable", "volumetric_fog"})
        CHECK(!IsToolMaterial(real));
    std::vector<std::pair<float, float>> uv = {{1562.25f, -40.5f}, {1563.0f, -39.75f}};
    RebaseUVs(uv);
    CHECK(uv[0].first == 0.25f && uv[0].second == 0.5f && uv[1].first == 1.0f && uv[1].second == 1.25f);
    for (float v : {0.1f, 123456.7f, -4.371139e-08f, 1e-30f, 3.0f})
    {
        std::string s = FormatFloat(v);
        CHECK(s.find('e') == std::string::npos);
        CHECK(std::strtof(s.c_str(), nullptr) == v);
    }
}

void TestGdt()
{
    const std::string gdt =
        "{\n"
        "\t\"model_a\" ( \"xmodel.gdf\" )\n\t{\n\t\t\"filename\" \"_custom\\\\props\\\\a.xmodel_bin\"\n\t\t\"note\" \"has } brace\"\n\t}\n"
        "\t\"model_b\" [ \"model_a\" ]\n\t{\n\t\t\"skinOverride\" \"mtl_a mtl_b\\r\\nmtl_c mtl_d\\r\\n\"\n\t}\n"
        "\t\"MODEL_C\" ( \"xmodel.gdf\" )\n\t{\n\t\t\"filename\" \"c.xmodel_export\"\n\t}\n"
        "}\n";
    auto entries = ScanGdt(gdt);
    CHECK(entries.size() == 3);
    if (entries.size() == 3)
    {
        CHECK(entries[0].name == "model_a" && entries[0].gdf == "xmodel.gdf" && !entries[0].derived);
        CHECK(entries[1].name == "model_b" && entries[1].gdf == "model_a" && entries[1].derived);
        std::string value;
        CHECK(GdtValue(gdt, entries[0], "filename", value) && value == "_custom\\props\\a.xmodel_bin");
        CHECK(GdtValue(gdt, entries[1], "skinOverride", value) && value == "mtl_a mtl_b\r\nmtl_c mtl_d\r\n");
        CHECK(!GdtValue(gdt, entries[0], "skinOverride", value));
    }
    std::string edited = gdt;
    RemoveGdtEntries(edited, "model_c", "xmodel.gdf");
    CHECK(edited.find("MODEL_C") == std::string::npos && edited.find("model_b") != std::string::npos);
    CHECK(ScanGdt(edited).size() == 2);
    CHECK(NextAssetName("rock") == "rock_1");
    CHECK(NextAssetName("rock_9") == "rock_10");
    CHECK(NextAssetName("rock_a") == "rock_a_1");
    CHECK(NextAssetName("rock_") == "rock__1");
}

std::string OneVertexModel(const char* offset)
{
    return std::string("MODEL\nVERSION 6\n\nNUMBONES 1\nBONE 0 -1 \"tag_origin\"\n\nBONE 0\nOFFSET 0, 0, 0\nSCALE 1, 1, 1\nX 1, 0, 0\nY 0, "
                       "1, 0\nZ 0, 0, 1\n\n"
                       "NUMVERTS 3\nVERT 0\nOFFSET ") +
           offset +
           "\nBONES 1\nBONE 0 1\n\nVERT 1\nOFFSET 0, 1, 0\nBONES 1\nBONE 0 1\n\nVERT 2\nOFFSET 0, 0, 1\nBONES 1\nBONE 0 1\n\n"
           "NUMFACES 1\nTRI 0 0 0 0\nVERT 0\nNORMAL 1, 0, 0\nCOLOR 1 1 1 1\nUV 1 0 0\nVERT 1\nNORMAL 1, 0, 0\nCOLOR 1 1 1 1\nUV 1 0 1\n"
           "VERT 2\nNORMAL 1, 0, 0\nCOLOR 1 1 1 1\nUV 1 1 0\n\nNUMOBJECTS 1\nOBJECT 0 \"m\"\n\nNUMMATERIALS 1\nMATERIAL 0 \"rock\" "
           "\"Lambert\" \"\"\n"
           "COLOR 0 0 0 1\n";
}

void TestXModel()
{
    // A cube: 12 triangles welded down to 8 corners.
    std::vector<Tex> tex;
    ParseTex(SplitBrushBlocks(kEntityText)["{aaaaaaaa-0000-0000-0000-000000000001}"], tex);
    float mins[3] = {0, 0, 0}, maxs[3] = {64, 64, 64};
    std::vector<Face> faces;
    CHECK(Reconstruct(mins, maxs, PlanesOf(tex), tex, faces));
    ExportStats stats;
    std::string text = BuildXModelExport(faces, {32, 32, 32}, stats);
    CHECK(stats.triangles == 12 && stats.vertices == 36);
    xm::Doc doc = xm::Parse(text);
    xm::Cleanup cleanup;
    std::string written = xm::Write(doc, cleanup);
    CHECK(cleanup.verticesAfter == 8 && cleanup.facesAfter == 12);
    CHECK(written.find("NUMVERTS 8\n") != std::string::npos);

    // Placement: yaw 90 and modelscale 2 move (1,0,0) to (10,2,0) relative to the pivot.
    xm::Doc base = xm::Normalized(xm::Parse(EmptyXModelExport(), true));
    xm::Placement pl;
    pl.origin[0] = 10;
    pl.angles[1] = 90;
    pl.scale = 2;
    xm::AppendReport report = xm::Append(base, xm::Parse(OneVertexModel("1, 0, 0")), pl);
    CHECK(std::string(report.strategy) == "baked_to_shared_root");
    std::string merged = xm::Write(base, cleanup);
    CHECK(merged.find("OFFSET 10.000000 2.000000 0.000000") != std::string::npos);

    // Mirror across X flips positions and normals textually and reverses the winding.
    xm::Doc mirror = xm::Parse(OneVertexModel("1.5, 0, 0"));
    xm::Bounds src, dst;
    xm::Mirror(mirror, 1, src, dst);
    std::string mirrored = xm::Write(mirror, cleanup);
    CHECK(mirrored.find("OFFSET -1.5, 0, 0") != std::string::npos && mirrored.find("NORMAL -1, 0, 0") != std::string::npos);
    CHECK(src.lo[0] == 0 && src.hi[0] == 1.5 && dst.lo[0] == -1.5 && dst.hi[0] == 0);
    size_t v1 = mirrored.find("NUMFACES"), a = mirrored.find("VERT 2", v1), b = mirrored.find("VERT 1", v1);
    CHECK(a != std::string::npos && b != std::string::npos && a < b);

    // Skin overrides rename materials in order, so a child's rule can follow its parent's.
    std::string skinned = OneVertexModel("1, 0, 0");
    xm::SkinResult skin = xm::ApplySkinOverrides(skinned, "rock stone\r\nstone marble\nmissing other\n");
    CHECK(skin.rules == 3 && skin.replaced == 2 && skin.unmatched.size() == 1);
    CHECK(skinned.find("MATERIAL 0 \"marble\" \"Lambert\"") != std::string::npos);
}

// Commands for RunTests.ps1 ----------------------------------------------------------------------------------------

// manifest: one model per line, "export ox oy oz cx cy cz pitch yaw roll"
int MergeMany(const char* basePath, const char* manifestPath, const char* outPath, bool allowEmpty)
{
    xm::Doc combined = xm::Normalized(xm::Parse(ReadAll(basePath), allowEmpty));
    std::istringstream manifest(ReadAll(manifestPath));
    std::string line;
    while (std::getline(manifest, line))
    {
        std::istringstream row(line);
        std::string exportPath;
        xm::Placement pl;
        if (!(row >> exportPath)) continue;
        row >> pl.origin[0] >> pl.origin[1] >> pl.origin[2] >> pl.center[0] >> pl.center[1] >> pl.center[2] >> pl.angles[0] >>
            pl.angles[1] >> pl.angles[2];
        xm::AppendReport r = xm::Append(combined, xm::Parse(ReadAll(exportPath.c_str())), pl);
        std::printf("%s strategy=%s bones=%zu verts=%zu\n", exportPath.c_str(), r.strategy, r.sourceBones, r.sourceVertices);
    }
    xm::Cleanup cleanup;
    WriteAll(outPath, xm::Write(combined, cleanup));
    std::printf("vertices %zu->%zu faces %zu->%zu\n", cleanup.verticesBefore, cleanup.verticesAfter, cleanup.facesBefore,
                cleanup.facesAfter);
    return 0;
}
int Optimize(const char* in, const char* out)
{
    xm::Doc doc = xm::Parse(ReadAll(in));
    xm::Cleanup cleanup;
    WriteAll(out, xm::Write(doc, cleanup));
    return 0;
}
int Mirror(const char* in, const char* axes, const char* out)
{
    xm::Doc doc = xm::Parse(ReadAll(in));
    xm::Bounds src, dst;
    xm::Mirror(doc, (unsigned)std::atoi(axes), src, dst);
    xm::Cleanup cleanup;
    WriteAll(out, xm::Write(doc, cleanup));
    std::printf("mirrored %s -> %s\n", xm::FormatBounds(src).c_str(), xm::FormatBounds(dst).c_str());
    return 0;
}
// A brush export of the test entity's two brushes and patch, the base for merge comparisons.
int BrushExport(const char* out)
{
    auto blocks = SplitBrushBlocks(kEntityText);
    std::vector<Face> faces;
    for (const char* guid : {"{aaaaaaaa-0000-0000-0000-000000000001}", "{bbbbbbbb-0000-0000-0000-000000000002}"})
    {
        std::vector<Tex> tex;
        ParseTex(blocks[guid], tex);
        float mins[3] = {0, 0, guid[1] == 'a' ? 0.0f : 32.0f}, maxs[3] = {64, 64, 64};
        Reconstruct(mins, maxs, PlanesOf(tex), tex, faces);
    }
    PatchGrid patch;
    ParsePatch(blocks["{cccccccc-0000-0000-0000-000000000003}"], "{CCCCCCCC-0000-0000-0000-000000000003}", patch);
    PatchFaces(patch, faces);
    faces.erase(std::remove_if(faces.begin(), faces.end(), [](const Face& f) { return IsToolMaterial(f.material); }), faces.end());
    ExportStats stats;
    WriteAll(out, BuildXModelExport(faces, {32, 32, 32}, stats));
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        if (argc >= 5 && !strcmp(argv[1], "merge-many"))
            return MergeMany(argv[2], argv[3], argv[4], argc >= 6 && !strcmp(argv[5], "--allow-empty-base"));
        if (argc == 4 && !strcmp(argv[1], "optimize")) return Optimize(argv[2], argv[3]);
        if (argc == 5 && !strcmp(argv[1], "mirror")) return Mirror(argv[2], argv[3], argv[4]);
        if (argc == 3 && !strcmp(argv[1], "brush-export")) return BrushExport(argv[2]);
        if (argc == 3 && !strcmp(argv[1], "empty-export"))
        {
            WriteAll(argv[2], EmptyXModelExport());
            return 0;
        }
        TestBrushBlocks();
        TestMaterialsAndUVs();
        TestGdt();
        TestXModel();
    }
    catch (const std::exception& e)
    {
        std::printf("ERROR %s\n", e.what());
        return 2;
    }
    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
