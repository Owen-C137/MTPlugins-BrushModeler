// Reading and editing GDT text. No Radiant dependencies, so the tests can build it alone.
#pragma once
#include <string>
#include <vector>

namespace bm
{
struct GdtEntry
{
    std::string name, gdf;
    size_t begin = 0, end = 0, bodyBegin = 0, bodyEnd = 0; // [begin, end) is the whole entry, the body excludes its braces
    bool derived = false;                                  // "name" [ "parent" ] instead of ( "type.gdf" )
};
// Every top-level entry of a GDT file. Unrecognised text is skipped, so a damaged entry does not hide the rest.
std::vector<GdtEntry> ScanGdt(const std::string& text);
// The value of "key" "value" in an entry's body, with \\, \n, \r and \t decoded.
bool GdtValue(const std::string& text, const GdtEntry& e, const char* key, std::string& value);
// Removes every non-derived entry of this name and type (case-insensitive) with its leading indentation and line break.
void RemoveGdtEntries(std::string& text, const std::string& name, const char* gdf);
// "name_3" -> "name_4", "name" -> "name_1".
std::string NextAssetName(const std::string& name);
} // namespace bm
