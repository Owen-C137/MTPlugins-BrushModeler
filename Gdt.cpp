// Reading and editing GDT text.
#include "Gdt.h"
#include <algorithm>
#include <cctype>
#include <cstring>

namespace bm
{
namespace
{
// Reads a quoted GDT string at text[pos] == '"'. GDT files use C escapes: paths double their backslashes and
// multi-line values such as skinOverride store "\r\n".
bool ReadString(const std::string& text, size_t& pos, std::string& out)
{
    if (pos >= text.size() || text[pos] != '"') return false;
    out.clear();
    for (size_t i = pos + 1; i < text.size(); ++i)
    {
        char c = text[i];
        if (c == '\\' && i + 1 < text.size())
        {
            char e = text[++i];
            out += e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e;
            continue;
        }
        if (c == '"')
        {
            pos = i + 1;
            return true;
        }
        out += c;
    }
    return false;
}
void SkipSpace(const std::string& text, size_t& pos)
{
    while (pos < text.size() && isspace((unsigned char)text[pos])) ++pos;
}
bool SameText(const std::string& a, const char* b) { return _stricmp(a.c_str(), b) == 0; }
} // namespace

std::vector<GdtEntry> ScanGdt(const std::string& text)
{
    std::vector<GdtEntry> entries;
    size_t pos = text.find('{');
    if (pos == std::string::npos) return entries;
    ++pos;
    while (pos < text.size())
    {
        SkipSpace(text, pos);
        if (pos >= text.size() || text[pos] == '}') break;
        GdtEntry e;
        e.begin = pos;
        size_t p = pos;
        bool ok = ReadString(text, p, e.name);
        if (ok)
        {
            SkipSpace(text, p);
            ok = p < text.size() && (text[p] == '(' || text[p] == '[');
            if (ok)
            {
                char close = text[p] == '(' ? ')' : ']';
                e.derived = close == ']';
                ++p;
                SkipSpace(text, p);
                ok = ReadString(text, p, e.gdf);
                SkipSpace(text, p);
                ok = ok && p < text.size() && text[p] == close;
                ++p;
            }
        }
        if (ok)
        {
            SkipSpace(text, p);
            ok = p < text.size() && text[p] == '{';
        }
        if (!ok)
        {
            ++pos;
            continue;
        }
        e.bodyBegin = ++p;
        int depth = 1;
        std::string ignored;
        while (p < text.size())
        {
            if (text[p] == '"')
            {
                if (!ReadString(text, p, ignored)) p = text.size();
                continue;
            }
            if (text[p] == '{') ++depth;
            if (text[p] == '}' && --depth == 0) break;
            ++p;
        }
        if (p >= text.size()) break;
        e.bodyEnd = p;
        e.end = p + 1;
        pos = e.end;
        entries.push_back(std::move(e));
    }
    return entries;
}

bool GdtValue(const std::string& text, const GdtEntry& e, const char* key, std::string& value)
{
    size_t p = e.bodyBegin;
    std::string k, v;
    while (p < e.bodyEnd)
    {
        SkipSpace(text, p);
        if (p >= e.bodyEnd || !ReadString(text, p, k)) return false;
        SkipSpace(text, p);
        if (!ReadString(text, p, v)) return false;
        if (SameText(k, key))
        {
            value = v;
            return true;
        }
    }
    return false;
}

void RemoveGdtEntries(std::string& text, const std::string& name, const char* gdf)
{
    auto entries = ScanGdt(text);
    // Back to front, so erasing one leaves the earlier offsets valid.
    for (auto it = entries.rbegin(); it != entries.rend(); ++it)
    {
        if (it->derived || !SameText(it->name, name.c_str()) || !SameText(it->gdf, gdf)) continue;
        size_t begin = it->begin;
        while (begin > 0 && (text[begin - 1] == ' ' || text[begin - 1] == '\t')) --begin;
        if (begin > 0 && text[begin - 1] == '\n') --begin;
        if (begin > 0 && text[begin - 1] == '\r') --begin;
        text.erase(begin, it->end - begin);
    }
}

std::string NextAssetName(const std::string& name)
{
    size_t us = name.find_last_of('_');
    if (us != std::string::npos && us + 1 < name.size() && name.size() - us - 1 <= 18 &&
        std::all_of(name.begin() + us + 1, name.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return name.substr(0, us) + "_" + std::to_string(std::stoull(name.substr(us + 1)) + 1);
    return name + "_1";
}
} // namespace bm
