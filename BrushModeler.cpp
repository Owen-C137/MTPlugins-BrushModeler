// BrushModeler: convert selected Radiant brushes and patches to a BO3 xmodel asset, merging selected models into it.
#define MODTOOLS_PLUGIN_IMPL
#include "modtools_plugin.hpp"
#include "Gdt.h"
#include "Geometry.h"
#include "Version.h"
#include "XModelExport.h"
#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace bm;
namespace fs = std::filesystem;

namespace
{
ModToolsPane g_pane{};
ModToolsWidget g_status{}, g_count{}, g_asset{}, g_gdt{}, g_outputDir{}, g_modelStatus{}, g_cancel{}, g_collision{}, g_pivot{},
    g_skipTools{}, g_mirrorX{}, g_mirrorY{}, g_mirrorZ{};
std::wstring g_log;

struct QueuedModel
{
    std::wstring source;  // .xmodel_bin, or an .xmodel_export the GDT points at directly
    std::wstring gdtPath; // the GDT that defines the model's source file (mirrored copies are added to it)
    std::string guid, asset;
    std::string skinOverride; // material remaps of a derived model, parent rules first
    std::array<float, 3> origin{}, angles{};
    float scale = 1;
};
enum class BuildMode
{
    Combined,
    Mirror
};
BuildMode g_buildMode = BuildMode::Combined;
std::vector<QueuedModel> g_queuedModels;

std::thread g_buildThread, g_lookupThread;
std::mutex g_buildMutex, g_fileLogMutex, g_lookupMutex;
std::atomic_bool g_buildWorkerDone{false}, g_lookupDone{false}, g_lookupCancelRequested{false}, g_buildCancelRequested{false},
    g_unloading{false};
std::atomic_int g_buildPhase{0};
bool g_buildRunning = false, g_waitingRadiantRefresh = false, g_lookupRunning = false;
ULONGLONG g_refreshReadyAt = 0, g_buildStartedAt = 0, g_bannerUpdatedAt = 0, g_lookupStartedAt = 0, g_lookupBannerAt = 0;
int g_phaseShown = -1;
std::vector<QueuedModel> g_lookupResults;
std::string g_lookupError;
// Worker results, read by FinishBuild under g_buildMutex.
std::string g_buildError, g_buildMap, g_buildAsset;
std::vector<std::string> g_buildQueuedGuids, g_buildBrushGuids;
volatile LONG g_exportStage = 0;

enum BuildPhase
{
    PhaseGeometry = 1,
    PhaseModels,
    PhaseCompile,
    PhaseGdt,
    PhaseDatabase,
    PhaseReplace
};

// ---- logging and text helpers ----------------------------------------------------------------------------------

void FileLog(const char* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_fileLogMutex);
    char msg[2048]{};
    va_list a;
    va_start(a, fmt);
    vsnprintf_s(msg, sizeof msg, _TRUNCATE, fmt, a);
    va_end(a);
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char line[2300]{};
    int n = _snprintf_s(line, sizeof line, _TRUNCATE, "%04u-%02u-%02u %02u:%02u:%02u.%03u [tid=%lu] %s\r\n", t.wYear, t.wMonth, t.wDay,
                        t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, GetCurrentThreadId(), msg);
    if (n <= 0) return;
    HANDLE h = CreateFileW(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE)
    {
        DWORD w = 0;
        WriteFile(h, line, (DWORD)n, &w, nullptr);
        CloseHandle(h);
    }
}
void Log(const char* fmt, ...)
{
    char msg[2048]{};
    va_list a;
    va_start(a, fmt);
    vsnprintf_s(msg, sizeof msg, _TRUNCATE, fmt, a);
    va_end(a);
    FileLog("%s", msg);
    rp::Log("%s", msg);
}
std::wstring Join(std::wstring a, const wchar_t* b)
{
    if (!a.empty() && a.back() != L'\\') a += L'\\';
    a += b;
    return a;
}
std::wstring Join(std::wstring a, const std::wstring& b) { return Join(std::move(a), b.c_str()); }
std::wstring Utf8Wide(const std::string& s)
{
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n ? n - 1 : 0, L'\0');
    if (n) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}
std::string WideUtf8(const std::wstring& s)
{
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string v(n ? n - 1 : 0, '\0');
    if (n) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), -1, &v[0], n, nullptr, nullptr);
    return v;
}
std::string LowerAscii(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    return s;
}
bool EndsWithNoCase(const std::wstring& s, const wchar_t* suffix)
{
    size_t n = wcslen(suffix);
    return s.size() >= n && _wcsicmp(s.c_str() + s.size() - n, suffix) == 0;
}
bool ReadFileText(const std::wstring& path, std::string& out, unsigned long long maxBytes = 512ull * 1024 * 1024)
{
    std::error_code ec;
    auto size = fs::file_size(path, ec);
    if (ec || size > maxBytes) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}
bool WriteFileText(const std::wstring& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(text.data(), (std::streamsize)text.size());
    out.flush();
    return (bool)out;
}

// ---- status banner ---------------------------------------------------------------------------------------------

enum class Tone
{
    Info,
    Busy,
    Ready,
    Warning,
    Error
};
void Status(Tone tone, const std::string& s, bool writeLog = true)
{
    struct Colors
    {
        const char *bg, *fg, *edge;
    };
    static const Colors colors[] = {{"#263744", "#c8e4f5", "#45a9e8"},
                                    {"#493b24", "#ffedc2", "#efad35"},
                                    {"#243d30", "#d5f6df", "#52c878"},
                                    {"#4a3320", "#ffe0c2", "#f08a3c"},
                                    {"#442629", "#ffd9d9", "#ec5b61"}};
    const Colors& c = colors[(int)tone];
    std::string escaped;
    escaped.reserve(s.size());
    for (char ch : s)
    {
        if (ch == '&')
            escaped += "&amp;";
        else if (ch == '<')
            escaped += "&lt;";
        else if (ch == '>')
            escaped += "&gt;";
        else
            escaped += ch;
    }
    std::string html = "<div style=\"background-color:" + std::string(c.bg) + ";color:" + c.fg + ";border:1px solid " + c.edge +
                       ";border-left:6px solid " + c.edge + ";border-radius:4px;padding:9px 11px;font-size:13px;font-weight:600;\">" +
                       escaped + "</div>";
    if (g_status && rp::host->ui) rp::host->ui->SetLabelText(g_status, html.c_str());
    if (writeLog) Log("%s", s.c_str());
}
void ModelStatus(const std::string& text)
{
    if (g_modelStatus && rp::host->ui) rp::host->ui->SetLabelText(g_modelStatus, text.c_str());
}
std::string Edit(ModToolsWidget w) { return rp::EditText(w); }
std::wstring GameRoot() { return rp::host->RadiantRoot ? rp::host->RadiantRoot(rp::self) : L""; }

// ---- options ---------------------------------------------------------------------------------------------------

enum Collision
{
    CollisionNone,
    CollisionHigh
};
enum Pivot
{
    PivotCenter,
    PivotBottomCenter
};
int ComboValue(ModToolsWidget combo, const char* key, int def)
{
    if (combo && rp::HasUi(&rp::host->ui->ComboCurrent))
    {
        int v = rp::host->ui->ComboCurrent(combo);
        if (v >= 0) return v;
    }
    return rp::ConfigGetInt("model", key, def);
}
bool SkipToolFaces()
{
    if (g_skipTools && rp::HasUi(&rp::host->ui->IsChecked)) return rp::host->ui->IsChecked(g_skipTools) != 0;
    return rp::ConfigGetBool("model", "skip_tool_faces", true);
}
void SaveOptions()
{
    rp::ConfigSet("model", "collision", std::to_string(ComboValue(g_collision, "collision", CollisionHigh)));
    rp::ConfigSet("model", "pivot", std::to_string(ComboValue(g_pivot, "pivot", PivotCenter)));
    rp::ConfigSet("model", "skip_tool_faces", SkipToolFaces() ? "1" : "0");
}

// ---- GDT files -------------------------------------------------------------------------------------------------

bool IsGdtTarget(const std::string& value)
{
    std::wstring path = Utf8Wide(value);
    if (path.empty()) return false;
    std::wstring extension = fs::path(path).extension().wstring();
    return _wcsicmp(extension.c_str(), L".gdt") == 0;
}
bool WriteGdtAtomically(const std::wstring& path, const std::string& contents, std::string& error)
{
    std::wstring extension = fs::path(path).extension().wstring();
    if (_wcsicmp(extension.c_str(), L".gdt") != 0)
    {
        error = "Refusing to write: the GDT target must end in .gdt.";
        return false;
    }
    std::wstring temp = path + L".brushmodeler.tmp", backup = path + L".brushmodeler.bak";
    if (!WriteFileText(temp, contents))
    {
        DeleteFileW(temp.c_str());
        error = "Could not write the temporary GDT update.";
        return false;
    }
    if (!CopyFileW(path.c_str(), backup.c_str(), FALSE))
    {
        DeleteFileW(temp.c_str());
        error = "Could not back up the GDT; the original file was left unchanged.";
        return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DWORD code = GetLastError();
        DeleteFileW(temp.c_str());
        error = "Could not safely replace the GDT; the original file was left unchanged (Windows error " + std::to_string(code) + ").";
        return false;
    }
    return true;
}

// gdt.db is the indexed asset catalog Radiant uses, read through the game's own SQLite build.
class GdtDb
{
  public:
    explicit GdtDb(const std::wstring& root)
    {
        module_ = LoadLibraryW((fs::path(root) / L"bin" / L"sqlite64r.dll").c_str());
        if (!module_) return;
        open_ = reinterpret_cast<OpenFn>(GetProcAddress(module_, "sqlite3_open_v2"));
        prepare_ = reinterpret_cast<PrepareFn>(GetProcAddress(module_, "sqlite3_prepare_v2"));
        bind_ = reinterpret_cast<BindFn>(GetProcAddress(module_, "sqlite3_bind_text"));
        step_ = reinterpret_cast<StepFn>(GetProcAddress(module_, "sqlite3_step"));
        column_ = reinterpret_cast<ColumnFn>(GetProcAddress(module_, "sqlite3_column_text"));
        finalize_ = reinterpret_cast<FinalizeFn>(GetProcAddress(module_, "sqlite3_finalize"));
        close_ = reinterpret_cast<CloseFn>(GetProcAddress(module_, "sqlite3_close"));
        if (!open_ || !prepare_ || !bind_ || !step_ || !column_ || !finalize_ || !close_) return;
        std::string file = WideUtf8((fs::path(root) / L"gdtdb" / L"gdt.db").wstring());
        const int readOnly = 0x00000001, fullMutex = 0x00010000;
        if (open_(file.c_str(), &db_, readOnly | fullMutex, nullptr) != 0 && db_)
        {
            close_(db_);
            db_ = nullptr;
        }
    }
    ~GdtDb()
    {
        if (db_) close_(db_);
        if (module_) FreeLibrary(module_);
    }
    GdtDb(const GdtDb&) = delete;
    GdtDb& operator=(const GdtDb&) = delete;
    bool Ok() const { return db_ != nullptr; }
    // The GDT (as indexed, often relative to the game root) that defines this xmodel, empty when none does.
    std::string XModelGdt(const std::string& asset)
    {
        if (!db_) return {};
        const char* sql = "SELECT t.name FROM _entity e JOIN _gdf g ON e.FK_gdf=g.PK_id JOIN _gdt t ON e.FK_gdt=t.PK_id "
                          "WHERE e.name=?1 COLLATE NOCASE AND g.name=?2";
        void* stmt = nullptr;
        std::string result;
        if (prepare_(db_, sql, -1, &stmt, nullptr) != 0) return {};
        bind_(stmt, 1, asset.c_str(), -1, nullptr);
        bind_(stmt, 2, "xmodel", -1, nullptr);
        if (step_(stmt) == 100)
            if (const unsigned char* value = column_(stmt, 0)) result = reinterpret_cast<const char*>(value);
        finalize_(stmt);
        return result;
    }

  private:
    using OpenFn = int (*)(const char*, void**, int, const char*);
    using PrepareFn = int (*)(void*, const char*, int, void**, const char**);
    using BindFn = int (*)(void*, int, const char*, int, void*);
    using StepFn = int (*)(void*);
    using ColumnFn = const unsigned char* (*)(void*, int);
    using FinalizeFn = int (*)(void*);
    using CloseFn = int (*)(void*);
    HMODULE module_ = nullptr;
    void* db_ = nullptr;
    OpenFn open_ = nullptr;
    PrepareFn prepare_ = nullptr;
    BindFn bind_ = nullptr;
    StepFn step_ = nullptr;
    ColumnFn column_ = nullptr;
    FinalizeFn finalize_ = nullptr;
    CloseFn close_ = nullptr;
};

// A name used by no entry of the target GDT and by no xmodel anywhere in gdt.db.
std::string UniqueAssetName(std::string name, const std::wstring& gdtPath, const std::wstring& root)
{
    std::unordered_set<std::string> names;
    std::string text;
    if (ReadFileText(gdtPath, text))
        for (const auto& e : ScanGdt(text)) names.insert(LowerAscii(e.name));
    GdtDb db(root);
    std::string original = name;
    for (int guard = 0; guard < 10000; ++guard)
    {
        bool taken = names.count(LowerAscii(name)) != 0;
        if (!taken && db.Ok())
        {
            std::string other = db.XModelGdt(name);
            if (!other.empty())
            {
                taken = true;
                Log("asset name %s is already an xmodel in %s", name.c_str(), other.c_str());
            }
        }
        if (!taken) break;
        name = NextAssetName(name);
    }
    if (name != original) Log("asset name collision resolved: requested=%s selected=%s", original.c_str(), name.c_str());
    return name;
}

// ---- external tools --------------------------------------------------------------------------------------------

// Runs a tool without a console, logging its exit code and the tail of its output. shouldStop ends it early.
int Run(const std::wstring& exe, const std::wstring& args, const std::wstring& cwd, const std::function<bool()>& shouldStop,
        DWORD timeoutMs = 300000)
{
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::string exeName = WideUtf8(fs::path(exe).filename().wstring());
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
    {
        FileLog("CreatePipe failed exe=%s error=%lu", exeName.c_str(), GetLastError());
        return -1;
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    // Hand the child only its own pipe and NUL, not every inheritable handle Radiant holds.
    HANDLE inherit[2] = {writePipe, nul};
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<unsigned char> attrBuffer(attrSize);
    auto attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrBuffer.data());
    bool haveAttrs = InitializeProcThreadAttributeList(attrs, 1, 0, &attrSize) &&
                     UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit,
                                               (nul != INVALID_HANDLE_VALUE ? 2 : 1) * sizeof(HANDLE), nullptr, nullptr);
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul != INVALID_HANDLE_VALUE ? nul : nullptr;
    si.StartupInfo.hStdOutput = writePipe;
    si.StartupInfo.hStdError = writePipe;
    si.lpAttributeList = haveAttrs ? attrs : nullptr;
    PROCESS_INFORMATION pi{};
    BOOL started =
        CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | (haveAttrs ? EXTENDED_STARTUPINFO_PRESENT : 0),
                       nullptr, cwd.c_str(), &si.StartupInfo, &pi);
    DWORD startError = GetLastError();
    if (haveAttrs) DeleteProcThreadAttributeList(attrs);
    CloseHandle(writePipe);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!started)
    {
        CloseHandle(readPipe);
        FileLog("CreateProcess failed exe=%s error=%lu", exeName.c_str(), startError);
        return -1;
    }
    CloseHandle(pi.hThread);
    std::string output;
    auto drain = [&] {
        DWORD available = 0;
        while (PeekNamedPipe(readPipe, nullptr, 0, nullptr, &available, nullptr) && available)
        {
            char buf[4096];
            DWORD got = 0;
            if (!ReadFile(readPipe, buf, (std::min)(available, (DWORD)sizeof buf), &got, nullptr) || !got) break;
            output.append(buf, got);
            if (output.size() > 256 * 1024) output.erase(0, output.size() - 128 * 1024);
        }
    };
    ULONGLONG started64 = GetTickCount64();
    const char* outcome = "completed";
    DWORD code = 999;
    for (;;)
    {
        DWORD wait = WaitForSingleObject(pi.hProcess, 50);
        drain();
        if (wait == WAIT_OBJECT_0)
        {
            GetExitCodeProcess(pi.hProcess, &code);
            break;
        }
        bool stop = shouldStop && shouldStop();
        if (stop || GetTickCount64() - started64 > timeoutMs || wait == WAIT_FAILED)
        {
            outcome = stop ? "cancelled" : wait == WAIT_FAILED ? "wait failed" : "timed out";
            TerminateProcess(pi.hProcess, 124);
            WaitForSingleObject(pi.hProcess, 5000);
            drain();
            code = (DWORD)-1;
            break;
        }
    }
    CloseHandle(pi.hProcess);
    CloseHandle(readPipe);
    FileLog("process %s exit=%ld exe=%s elapsed_ms=%llu", outcome, (long)code, exeName.c_str(), GetTickCount64() - started64);
    if (!output.empty())
    {
        // The tail is what explains a failure; long successful runs only need the last lines.
        const size_t keep = code == 0 ? 1500 : 8000;
        if (output.size() > keep) output = "..." + output.substr(output.size() - keep);
        std::istringstream lines(output);
        std::string line;
        while (std::getline(lines, line))
        {
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            if (!line.empty()) FileLog("  [%s] %s", exeName.c_str(), line.c_str());
        }
    }
    return code == (DWORD)-1 ? -1 : (int)code;
}

// ---- Radiant 3D view refresh -----------------------------------------------------------------------------------

// The addresses below come from this one Radiant_modtools.exe build; another build would make them point at
// unrelated code, so the refresh only runs when the running executable is that build.
constexpr DWORD kKnownRadiantTimestamp = 0x5df45023;
bool RadiantBuildIsKnown()
{
    auto* base = reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
    if (!base) return false;
    auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE && nt->FileHeader.TimeDateStamp == kKnownRadiantTimestamp;
}
using ReloadCameraAssetsFn = __int64(__fastcall*)(void*, unsigned char);
bool ForceCameraAssetRefreshSeh(void** mainWinPtr, void* reloadAddress)
{
    __try
    {
        if (!mainWinPtr || !*mainWinPtr || !reloadAddress) return false;
        auto* mainWin = reinterpret_cast<unsigned char*>(*mainWinPtr);
        auto* children = *reinterpret_cast<unsigned char**>(mainWin + 64);
        if (!children) return false;
        auto* cam = *reinterpret_cast<unsigned char**>(children + 240);
        if (!cam) return false;
        reinterpret_cast<ReloadCameraAssetsFn>(reloadAddress)(cam, 1);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}
bool ForceRadiantAssetRefresh()
{
    if (!rp::Has(&rp::host->RadiantAddress) || !rp::host->RadiantAddress) return false;
    if (!RadiantBuildIsKnown())
    {
        Log("Radiant camera asset refresh skipped: unrecognised Radiant_modtools.exe build");
        return false;
    }
    auto** mainWinPtr = static_cast<void**>(rp::host->RadiantAddress(rp::self, 0x1444C6560));
    auto* reload = rp::host->RadiantAddress(rp::self, 0x140238300);
    bool ok = ForceCameraAssetRefreshSeh(mainWinPtr, reload);
    Log("Radiant camera asset refresh %s", ok ? "requested" : "unavailable or failed");
    return ok;
}

// ---- selected models -------------------------------------------------------------------------------------------

ModToolsEntity WorldEntity()
{
    return rp::Has(&rp::host->WorldEntity) && rp::host->WorldEntity ? rp::host->WorldEntity(rp::self) : nullptr;
}
// "xmodel,foo" or "/foo" -> "foo", empty for brush models ("*1") and entities without a model.
std::string ModelAssetName(std::string model)
{
    if (model.empty() || model[0] == '*') return {};
    if (model.rfind("xmodel,", 0) == 0) model.erase(0, 7);
    while (!model.empty() && (model[0] == '/' || model[0] == '\\')) model.erase(model.begin());
    return model;
}
std::vector<std::string> SelectedEntityGuids(const std::vector<ModToolsSelectedBrush>& selection)
{
    std::vector<std::string> ids;
    std::unordered_set<std::string> seen;
    for (const auto& s : selection)
        if (s.entityGuid[0] && seen.insert(s.entityGuid).second) ids.emplace_back(s.entityGuid);
    return ids;
}
bool IsQueued(const char* entityGuid)
{
    return std::any_of(g_queuedModels.begin(), g_queuedModels.end(), [&](const QueuedModel& m) { return m.guid == entityGuid; });
}
// Selected model entities, read with one entity text each. Worldspawn is skipped: its text is the whole map.
std::vector<QueuedModel> SelectedModels(const std::vector<ModToolsSelectedBrush>& selection)
{
    std::vector<std::string> ids = SelectedEntityGuids(selection);
    auto entities = rp::FindEntitiesByGuid(ids);
    ModToolsEntity world = WorldEntity();
    std::vector<QueuedModel> models;
    for (size_t i = 0; i < ids.size() && i < entities.size(); ++i)
    {
        if (!entities[i] || entities[i] == world) continue;
        rp::KeyValues keys = rp::GetKeys(entities[i]);
        if (rp::FindKey(keys, "classname") == "worldspawn") continue;
        std::string asset = ModelAssetName(rp::FindKey(keys, "model"));
        if (asset.empty()) continue;
        QueuedModel q;
        q.guid = ids[i];
        q.asset = asset;
        sscanf_s(rp::FindKey(keys, "origin").c_str(), "%f %f %f", &q.origin[0], &q.origin[1], &q.origin[2]);
        if (sscanf_s(rp::FindKey(keys, "angles").c_str(), "%f %f %f", &q.angles[0], &q.angles[1], &q.angles[2]) < 3)
        {
            q.angles = {0, 0, 0};
            sscanf_s(rp::FindKey(keys, "angle").c_str(), "%f", &q.angles[1]);
        }
        float scale = 1;
        if (sscanf_s(rp::FindKey(keys, "modelscale").c_str(), "%f", &scale) == 1 && std::isfinite(scale) && scale > 0) q.scale = scale;
        models.push_back(std::move(q));
    }
    return models;
}
std::vector<QueuedModel> UnqueuedSelectedModels(const std::vector<ModToolsSelectedBrush>& selection)
{
    std::vector<QueuedModel> found;
    for (auto& m : SelectedModels(selection))
        if (!IsQueued(m.guid.c_str())) found.push_back(std::move(m));
    return found;
}

// One parsed source GDT, reused until the file changes (lookup thread only).
struct GdtIndex
{
    fs::file_time_type modified{};
    uintmax_t size = 0;
    std::unordered_map<std::string, std::wstring> sources;       // lowercase asset -> its file
    std::unordered_map<std::string, std::string> parents, skins; // derived asset -> parent, asset -> skinOverride
};
std::unordered_map<std::string, GdtIndex> g_gdtIndexes;
struct ResolvedModel
{
    std::wstring source, gdtPath;
    std::string skinOverride;
};
std::unordered_map<std::string, ResolvedModel> g_resolvedModels;

std::string GdtKey(const fs::path& path) { return LowerAscii(WideUtf8(path.lexically_normal().wstring())); }
bool GdtStamp(const fs::path& path, fs::file_time_type& modified, uintmax_t& size)
{
    std::error_code ec;
    size = fs::file_size(path, ec);
    if (ec) return false;
    modified = fs::last_write_time(path, ec);
    return !ec;
}
bool GdtUnchanged(const std::wstring& gdtPath)
{
    auto it = g_gdtIndexes.find(GdtKey(gdtPath));
    fs::file_time_type modified;
    uintmax_t size;
    return it != g_gdtIndexes.end() && GdtStamp(gdtPath, modified, size) && it->second.modified == modified && it->second.size == size;
}
const GdtIndex* LoadGdtIndex(const fs::path& path, const std::wstring& root)
{
    fs::file_time_type modified;
    uintmax_t size;
    if (!GdtStamp(path, modified, size) || size > 64ull * 1024 * 1024) return nullptr;
    std::string key = GdtKey(path);
    auto cached = g_gdtIndexes.find(key);
    if (cached != g_gdtIndexes.end() && cached->second.modified == modified && cached->second.size == size)
    {
        FileLog("model GDT index cache hit: gdt=%s entries=%zu", WideUtf8(path.wstring()).c_str(), cached->second.sources.size());
        return &cached->second;
    }
    ULONGLONG started = GetTickCount64();
    std::string text;
    if (!ReadFileText(path.wstring(), text)) return nullptr;
    GdtIndex index;
    index.modified = modified;
    index.size = size;
    for (const auto& e : ScanGdt(text))
    {
        if (!e.derived && _stricmp(e.gdf.c_str(), "xmodel.gdf") != 0) continue;
        std::string name = LowerAscii(e.name), filename, skin;
        if (GdtValue(text, e, "skinOverride", skin) && !skin.empty()) index.skins[name] = skin;
        if (GdtValue(text, e, "filename", filename) && !filename.empty())
        {
            std::wstring rel = Utf8Wide(filename);
            std::replace(rel.begin(), rel.end(), L'/', L'\\');
            fs::path relative(rel);
            index.sources[name] = (relative.is_absolute() ? relative : fs::path(root) / L"model_export" / relative).wstring();
        }
        else if (e.derived)
            index.parents[name] = LowerAscii(e.gdf); // a derived entry's bracket holds its parent's name
    }
    FileLog("model GDT index cached: gdt=%s entries=%zu derived=%zu bytes=%llu parse_ms=%llu", WideUtf8(path.wstring()).c_str(),
            index.sources.size(), index.parents.size(), (unsigned long long)size, GetTickCount64() - started);
    return &(g_gdtIndexes[key] = std::move(index));
}

// Finds an xmodel's source file through gdt.db and its GDT, following derived entries ("child" [ "parent" ])
// within that GDT and, for a parent defined elsewhere, through gdt.db again.
bool FindModelSource(const std::string& asset, const std::wstring& root, GdtDb& db, ResolvedModel& out, const std::atomic_bool& cancel,
                     int depth = 0)
{
    if (cancel.load(std::memory_order_relaxed)) return false;
    std::string key = LowerAscii(asset);
    if (auto cached = g_resolvedModels.find(key); cached != g_resolvedModels.end())
    {
        std::error_code ec;
        if (fs::exists(cached->second.source, ec) && GdtUnchanged(cached->second.gdtPath))
        {
            out = cached->second;
            FileLog("model source cache hit: asset=%s", asset.c_str());
            return true;
        }
        g_resolvedModels.erase(cached);
    }
    if (depth > 16)
    {
        FileLog("derived model chain too deep or cyclic: asset=%s", asset.c_str());
        return false;
    }
    ULONGLONG started = GetTickCount64();
    std::string indexed = db.Ok() ? db.XModelGdt(asset) : std::string();
    if (indexed.empty())
    {
        FileLog("model not in gdt.db: asset=%s database=%s", asset.c_str(), db.Ok() ? "open" : "unavailable");
        return false;
    }
    fs::path gdtPath = Utf8Wide(indexed);
    if (!gdtPath.is_absolute()) gdtPath = fs::path(root) / gdtPath;
    const GdtIndex* index = LoadGdtIndex(gdtPath, root);
    if (!index)
    {
        FileLog("model GDT could not be read: asset=%s gdt=%s", asset.c_str(), WideUtf8(gdtPath.wstring()).c_str());
        return false;
    }
    std::vector<std::string> chain;
    std::unordered_set<std::string> visited;
    std::wstring source;
    std::string externalParent, name = key;
    while (true)
    {
        if (cancel.load(std::memory_order_relaxed)) return false;
        if (!visited.insert(name).second)
        {
            FileLog("derived model cycle detected: asset=%s at=%s", asset.c_str(), name.c_str());
            return false;
        }
        chain.push_back(name);
        if (auto s = index->sources.find(name); s != index->sources.end())
        {
            source = s->second;
            break;
        }
        auto parent = index->parents.find(name);
        if (parent == index->parents.end())
        {
            if (chain.size() == 1)
            {
                FileLog("model GDT has no source file for asset=%s gdt=%s", asset.c_str(), WideUtf8(gdtPath.wstring()).c_str());
                return false;
            }
            externalParent = name;
            chain.pop_back();
            break;
        }
        name = parent->second;
    }
    // Parent rules first, so a child's remap of a parent's material applies after it.
    std::string skin;
    auto addSkin = [&](const std::string& rules) {
        if (rules.empty()) return;
        if (!skin.empty()) skin += "\n";
        skin += rules;
    };
    ResolvedModel result;
    if (!externalParent.empty())
    {
        ResolvedModel parent;
        if (!FindModelSource(externalParent, root, db, parent, cancel, depth + 1))
        {
            FileLog("derived model parent lookup failed: requested=%s parent=%s", asset.c_str(), externalParent.c_str());
            return false;
        }
        addSkin(parent.skinOverride);
        result.source = parent.source;
        result.gdtPath = parent.gdtPath;
    }
    else
    {
        std::error_code ec;
        if (!fs::exists(source, ec))
        {
            FileLog("model GDT entry found but its file is missing: asset=%s file=%s", asset.c_str(), WideUtf8(source).c_str());
            return false;
        }
        result.source = source;
        result.gdtPath = gdtPath.wstring();
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        if (auto s = index->skins.find(*it); s != index->skins.end()) addSkin(s->second);
    result.skinOverride = skin;
    g_resolvedModels[key] = result;
    out = result;
    std::string chainText;
    for (const auto& part : chain) chainText += (chainText.empty() ? "" : " -> ") + part;
    if (!externalParent.empty()) chainText += " -> " + externalParent + " (other GDT)";
    FileLog("model GDT resolved: asset=%s chain=%s skin_override_chars=%zu gdt=%s source=%s elapsed_ms=%llu", asset.c_str(),
            chainText.c_str(), skin.size(), WideUtf8(out.gdtPath).c_str(), WideUtf8(out.source).c_str(), GetTickCount64() - started);
    return true;
}

void ShowCancel(bool visible)
{
    if (!g_cancel || !rp::host->ui) return;
    rp::host->ui->SetEnabled(g_cancel, 1);
    rp::host->ui->SetVisible(g_cancel, visible ? 1 : 0);
}
void OnCancel(void*)
{
    if (g_lookupRunning)
    {
        g_lookupCancelRequested.store(true, std::memory_order_relaxed);
        Status(Tone::Busy, "Cancelling model search… waiting for the current file check to finish.");
        FileLog("model source lookup cancellation requested");
    }
    else if (g_buildRunning)
    {
        g_buildCancelRequested.store(true, std::memory_order_relaxed);
        Status(Tone::Busy, "Cancelling the build… stopping the current step.");
        FileLog("build cancellation requested");
    }
    else
        return;
    if (rp::host->ui) rp::host->ui->SetEnabled(g_cancel, 0);
}
void FinishModelLookup()
{
    if (!g_lookupRunning || !g_lookupDone.load(std::memory_order_acquire)) return;
    if (g_lookupThread.joinable()) g_lookupThread.join();
    std::vector<QueuedModel> found;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(g_lookupMutex);
        found = std::move(g_lookupResults);
        error = std::move(g_lookupError);
    }
    g_lookupRunning = false;
    g_lookupDone.store(false, std::memory_order_release);
    ShowCancel(false);
    if (g_lookupCancelRequested.exchange(false, std::memory_order_relaxed))
    {
        Status(Tone::Info, "Model search cancelled. Previously queued models were kept.");
        ModelStatus("Model search cancelled. Previously queued models were kept.");
        return;
    }
    if (!error.empty())
    {
        ModelStatus(error);
        Status(Tone::Error, error);
        return;
    }
    g_queuedModels = std::move(found);
    std::string label = "Models ready to merge: " + std::to_string(g_queuedModels.size());
    ModelStatus(label);
    Status(Tone::Ready, label);
    for (const auto& m : g_queuedModels)
        FileLog("selected model queued guid=%s asset=%s origin=%.3f %.3f %.3f angles=%.3f %.3f %.3f scale=%.4f", m.guid.c_str(),
                m.asset.c_str(), m.origin[0], m.origin[1], m.origin[2], m.angles[0], m.angles[1], m.angles[2], m.scale);
}
void OnQueueModel(void*)
{
    try
    {
        if (g_lookupRunning)
        {
            Status(Tone::Info, "Model lookup is already running.");
            return;
        }
        if (g_buildRunning)
        {
            Status(Tone::Warning, "Wait for the current model build to finish before queuing models.");
            return;
        }
        std::vector<QueuedModel> requested = SelectedModels(rp::GetSelection());
        if (requested.empty())
        {
            Status(Tone::Warning, "Select one or more Radiant models, then click Queue selected models for merge.");
            return;
        }
        if (g_lookupThread.joinable()) g_lookupThread.join();
        g_lookupCancelRequested.store(false, std::memory_order_relaxed);
        g_lookupDone.store(false, std::memory_order_release);
        g_lookupRunning = true;
        g_lookupStartedAt = GetTickCount64();
        g_lookupBannerAt = 0;
        ShowCancel(true);
        size_t requestedCount = requested.size();
        std::wstring root = GameRoot();
        g_lookupThread = std::thread([root, requested = std::move(requested)]() mutable {
            std::string error;
            FileLog("model source lookup started: selected_models=%zu", requested.size());
            try
            {
                GdtDb db(root);
                std::vector<std::string> missing;
                for (auto& m : requested)
                {
                    if (g_lookupCancelRequested.load(std::memory_order_relaxed)) break;
                    ResolvedModel resolved;
                    if (!FindModelSource(m.asset, root, db, resolved, g_lookupCancelRequested))
                    {
                        if (!g_lookupCancelRequested.load(std::memory_order_relaxed)) missing.push_back(m.asset);
                        continue;
                    }
                    m.source = resolved.source;
                    m.gdtPath = resolved.gdtPath;
                    m.skinOverride = resolved.skinOverride;
                }
                if (!missing.empty())
                {
                    std::string names;
                    for (size_t i = 0; i < missing.size() && i < 6; ++i) names += (i ? ", " : "") + missing[i];
                    if (missing.size() > 6) names += ", and " + std::to_string(missing.size() - 6) + " more";
                    error = "Could not find " + std::to_string(missing.size()) + " model(s) in BO3's GDT database: " + names +
                            ". Confirm they are loaded in APE and the asset database is up to date. Nothing was queued.";
                }
            }
            catch (...)
            {
                error = "Model source lookup failed. See BrushModeler.log.";
                FileLog("exception while searching selected model GDTs");
            }
            {
                std::lock_guard<std::mutex> lock(g_lookupMutex);
                g_lookupResults = std::move(requested);
                g_lookupError = std::move(error);
            }
            g_lookupDone.store(true, std::memory_order_release);
        });
        std::string label = "Searching source GDTs for " + std::to_string(requestedCount) + " model(s)...";
        ModelStatus(label);
        Status(Tone::Busy, label);
    }
    catch (...)
    {
        Status(Tone::Error, "Could not read selected models. See BrushModeler.log.");
        Log("exception while starting model lookup");
    }
}

// ---- browsing --------------------------------------------------------------------------------------------------

int CALLBACK OutputFolderBrowseCallback(HWND hwnd, UINT msg, LPARAM, LPARAM data)
{
    if (msg == BFFM_INITIALIZED && data) SendMessageW(hwnd, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}
std::wstring DefaultOutputDir() { return Join(Join(GameRoot(), L"model_export"), L"_brush_modeler"); }
void OnBrowseOutput(void*)
{
    std::wstring current = Utf8Wide(Edit(g_outputDir));
    if (current.empty()) current = DefaultOutputDir();
    std::wstring title = L"Choose the folder for generated XModel files";
    BROWSEINFOW bi{};
    bi.hwndOwner = (HWND)rp::host->MainWindow(rp::self);
    bi.lpszTitle = title.c_str();
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn = OutputFolderBrowseCallback;
    bi.lParam = (LPARAM)current.c_str();
    PIDLIST_ABSOLUTE id = SHBrowseForFolderW(&bi);
    if (!id) return;
    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(id, path))
    {
        std::string value = WideUtf8(path);
        rp::host->ui->SetEditText(g_outputDir, value.c_str());
        rp::ConfigSet("model", "output_dir", value);
        Log("custom output folder selected: %s", value.c_str());
    }
    CoTaskMemFree(id);
}
void OnBrowseGdt(void*)
{
    wchar_t file[MAX_PATH]{};
    OPENFILENAMEW d{};
    d.lStructSize = sizeof d;
    d.hwndOwner = (HWND)rp::host->MainWindow(rp::self);
    d.lpstrFilter = L"GDT files (*.gdt)\0*.gdt\0All files (*.*)\0*.*\0";
    d.lpstrFile = file;
    d.nMaxFile = MAX_PATH;
    d.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&d))
    {
        rp::host->ui->SetEditText(g_gdt, WideUtf8(file).c_str());
        rp::ConfigSet("model", "gdt", WideUtf8(file));
    }
}

// ---- geometry capture (main thread) ----------------------------------------------------------------------------

// What the worker needs to rebuild one selected brush or patch without touching Radiant.
struct CapturedBrush
{
    std::string guid;
    float mins[3]{}, maxs[3]{};
    std::vector<float> planes;
    std::shared_ptr<const std::string> text; // the brush's own block, or its whole entity text when no block matched
    bool scoped = false;
};

bool CaptureSelection(const std::vector<ModToolsSelectedBrush>& sel, const std::vector<ModToolsBrush>& brushes,
                      std::vector<CapturedBrush>& out, std::string& err)
{
    InterlockedExchange(&g_exportStage, 1);
    if (sel.size() > 2000)
    {
        err = "Selection is too large (maximum 2,000 brushes per model).";
        return false;
    }
    std::vector<std::string> entityGuids = SelectedEntityGuids(sel);
    auto entityHandles = rp::FindEntitiesByGuid(entityGuids);
    std::unordered_map<std::string, ModToolsEntity> entitiesByGuid;
    for (size_t i = 0; i < entityGuids.size() && i < entityHandles.size(); ++i)
        if (entityHandles[i]) entitiesByGuid.emplace(entityGuids[i], entityHandles[i]);
    std::unordered_map<std::string, std::vector<ModToolsBrush>> brushesByEntity;
    for (size_t i = 0; i < sel.size(); ++i)
        if (brushes[i] && sel[i].entityGuid[0]) brushesByEntity[sel[i].entityGuid].push_back(brushes[i]);
    // One text write per owner entity, holding only the selected brushes, split into per-brush blocks so each
    // face is matched against its own brush's texture lines.
    InterlockedExchange(&g_exportStage, 2);
    std::unordered_map<std::string, std::shared_ptr<const std::string>> entityText;
    std::unordered_map<std::string, std::shared_ptr<const std::string>> blocks;
    for (auto& group : brushesByEntity)
    {
        auto entity = entitiesByGuid.find(group.first);
        if (entity == entitiesByGuid.end()) continue;
        std::string text;
        if (!rp::WriteEntityTextWith(entity->second, group.second, text)) continue;
        for (auto& block : SplitBrushBlocks(text))
            blocks.emplace(block.first, std::make_shared<const std::string>(std::move(block.second)));
        entityText.emplace(group.first, std::make_shared<const std::string>(std::move(text)));
    }
    Log("material capture batched: brushes=%zu owners=%zu blocks=%zu", sel.size(), brushesByEntity.size(), blocks.size());
    InterlockedExchange(&g_exportStage, 3);
    for (size_t i = 0; i < sel.size(); ++i)
    {
        if (!brushes[i])
        {
            Log("brush lookup failed guid=%s", sel[i].brushGuid);
            continue;
        }
        CapturedBrush c;
        c.guid = sel[i].brushGuid;
        std::copy(sel[i].mins, sel[i].mins + 3, c.mins);
        std::copy(sel[i].maxs, sel[i].maxs + 3, c.maxs);
        InterlockedExchange(&g_exportStage, 4);
        c.planes = rp::BrushPlanes(brushes[i]);
        if (auto b = blocks.find(LowerAscii(c.guid)); b != blocks.end())
        {
            c.text = b->second;
            c.scoped = true;
        }
        else if (auto t = entityText.find(sel[i].entityGuid); t != entityText.end())
            c.text = t->second;
        out.push_back(std::move(c));
    }
    InterlockedExchange(&g_exportStage, 5);
    return true;
}
bool CaptureSelectionProtected(const std::vector<ModToolsSelectedBrush>& sel, const std::vector<ModToolsBrush>& brushes,
                               std::vector<CapturedBrush>& out, std::string& err)
{
    __try
    {
        return CaptureSelection(sel, brushes, out, err);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        LONG stage = InterlockedCompareExchange(&g_exportStage, 0, 0);
        FileLog("structured exception in geometry capture: code=0x%08lX stage=%ld", GetExceptionCode(), stage);
        err = "Radiant hit an access violation while reading the selected geometry (stage logged). See BrushModeler.log.";
        return false;
    }
}

// ---- background build ------------------------------------------------------------------------------------------

struct BuildJob
{
    std::wstring root, outDir, pluginDir, gdtPath;
    std::string asset, gdtOutputPath, collisionLod;
    std::vector<CapturedBrush> brushes;
    std::vector<QueuedModel> models;
    Vec pivot{};
    bool skipTools = true, clean = true;
};
struct MirrorJob
{
    std::wstring root, outDir, pluginDir;
    std::string asset, gdtOutputPath, collisionLod;
    QueuedModel model;
    unsigned axes = 1;
    bool clean = true;
};
// cleanExports in BrushModeler.ini: remove generated exports and staged files after a successful build.
bool CleanExports()
{
    std::string value = LowerAscii(rp::ConfigGet("model", "cleanExports", "true"));
    return value == "true" || value == "1" || value == "yes" || value == "on";
}

// Rebuilds the captured brushes and patches as faces (worker thread). converted receives the GUIDs of the brushes
// that ended up in the model; the rest (failed, or only tool faces such as clip) stay in the map.
std::vector<Face> BuildFaces(const std::vector<CapturedBrush>& captured, bool skipTools, std::vector<std::string>& converted)
{
    std::vector<Face> faces;
    std::unordered_map<const std::string*, std::vector<Tex>> texCache;
    size_t patches = 0, brushes = 0, failed = 0, toolFaces = 0, untextured = 0, kept = 0;
    auto finishItem = [&](const CapturedBrush& c, size_t first) {
        if (skipTools)
        {
            size_t before = faces.size();
            faces.erase(std::remove_if(faces.begin() + first, faces.end(), [](const Face& f) { return IsToolMaterial(f.material); }),
                        faces.end());
            toolFaces += before - faces.size();
        }
        if (faces.size() > first)
            converted.push_back(c.guid);
        else
        {
            ++kept;
            FileLog("brush left in the map: guid=%s has no visible faces", c.guid.c_str());
        }
    };
    for (const auto& c : captured)
    {
        size_t first = faces.size();
        PatchGrid patch;
        if (c.text && ParsePatch(*c.text, c.guid.c_str(), patch))
        {
            if (PatchFaces(patch, faces))
            {
                ++patches;
                FileLog("patch captured guid=%s type=%s grid=%dx%d triangles=%zu material=%s", c.guid.c_str(),
                        patch.curve ? "curve" : "mesh", patch.rows, patch.cols, faces.size() - first, patch.material.c_str());
            }
            else
            {
                ++failed;
                FileLog("patch tessellation failed guid=%s type=%s grid=%dx%d", c.guid.c_str(), patch.curve ? "curve" : "mesh", patch.rows,
                        patch.cols);
            }
            finishItem(c, first);
            continue;
        }
        static const std::vector<Tex> noTex;
        const std::vector<Tex>* tex = &noTex;
        if (c.text)
        {
            auto it = texCache.find(c.text.get());
            if (it == texCache.end())
            {
                it = texCache.emplace(c.text.get(), std::vector<Tex>()).first;
                ParseTex(*c.text, it->second);
            }
            tex = &it->second;
        }
        size_t missing = 0;
        if (!Reconstruct(c.mins, c.maxs, c.planes, *tex, faces, &missing))
        {
            ++failed;
            FileLog("brush reconstruction failed guid=%s planes=%zu text=%s", c.guid.c_str(), c.planes.size() / 9,
                    !c.text    ? "none"
                    : c.scoped ? "brush"
                               : "entity");
            finishItem(c, first);
            continue;
        }
        ++brushes;
        if (missing)
        {
            untextured += missing;
            FileLog("brush faces without a texture line guid=%s faces=%zu (exported as caulk)", c.guid.c_str(), missing);
        }
        finishItem(c, first);
    }
    FileLog("geometry rebuilt: brushes=%zu patches=%zu failed=%zu left_in_map=%zu faces=%zu tool_faces_skipped=%zu untextured_faces=%zu",
            brushes, patches, failed, kept, faces.size(), toolFaces, untextured);
    return faces;
}

// A per-build folder for intermediate files. It is removed afterwards unless the build failed or cleanExports is off,
// so a failed build leaves its inputs for debugging.
class Staging
{
  public:
    Staging(const std::wstring& outDir, const char* purpose)
        : dir_(Join(outDir, L"_brushmodeler_" + Utf8Wide(purpose) + L"_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
                                std::to_wstring(GetTickCount64())))
    {
        std::error_code ec;
        ok_ = fs::create_directories(dir_, ec) && !ec;
    }
    ~Staging()
    {
        std::error_code ec;
        if (keep_)
            FileLog("intermediate files kept in %s", WideUtf8(dir_).c_str());
        else
            fs::remove_all(dir_, ec);
    }
    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;
    bool Ok() const { return ok_; }
    const std::wstring& Dir() const { return dir_; }
    void Finish(bool succeeded, bool clean) { keep_ = !succeeded || !clean; }

  private:
    std::wstring dir_;
    bool ok_ = false, keep_ = true;
};

// Turns one queued model into export text: an .xmodel_export is read directly, an .xmodel_bin goes through ExportX.
// A derived model's skinOverride is applied to its materials.
bool ModelExportText(const QueuedModel& m, size_t index, const std::wstring& pluginDir, const std::wstring& stagingDir,
                     const std::function<bool()>& stop, std::string& text, std::string& error)
{
    auto applySkin = [&] {
        if (m.skinOverride.empty()) return;
        xm::SkinResult skin = xm::ApplySkinOverrides(text, m.skinOverride);
        FileLog("skin overrides applied: asset=%s mappings=%zu material_slots_replaced=%zu", m.asset.c_str(), skin.rules, skin.replaced);
        for (const auto& rule : skin.unmatched)
            FileLog("skin override source material absent from export: asset=%s rule=%s", m.asset.c_str(), rule.c_str());
    };
    if (EndsWithNoCase(m.source, L".xmodel_export"))
    {
        if (!ReadFileText(m.source, text))
        {
            error = "Could not read the export of selected model " + m.asset + ".";
            return false;
        }
        applySkin();
        return true;
    }
    std::wstring exportx = Join(Join(pluginDir, L"Tools"), L"exportx.exe");
    if (GetFileAttributesW(exportx.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        error = "Brush Modeler could not find its bundled ExportX tool. Reinstall the plugin folder.";
        FileLog("bundled ExportX missing path=%s", WideUtf8(exportx).c_str());
        return false;
    }
    std::wstring name = L"source_" + std::to_wstring(index), sourceBin = Join(stagingDir, name + L".xmodel_bin"),
                 sourceExp = Join(stagingDir, name + L".xmodel_export");
    if (!CopyFileW(m.source.c_str(), sourceBin.c_str(), FALSE))
    {
        error = "Could not stage selected model " + m.asset + ". See BrushModeler.log.";
        FileLog("model staging copy failed asset=%s error=%lu", m.asset.c_str(), GetLastError());
        return false;
    }
    if (Run(exportx, L"-f \"" + name + L".xmodel_bin\" -m export", stagingDir, stop, 120000) != 0 || !ReadFileText(sourceExp, text))
    {
        error = "ExportX could not export selected model " + m.asset + ". See BrushModeler.log.";
        return false;
    }
    applySkin();
    return true;
}

bool BuildCancelled() { return g_buildCancelRequested.load(std::memory_order_relaxed) || g_unloading.load(std::memory_order_relaxed); }
bool Unloading() { return g_unloading.load(std::memory_order_relaxed); }

// Writes the export, compiles it with export2bin, adds its xmodel entry to the GDT and refreshes gdt.db.
bool CompileAndRegister(const std::wstring& root, const std::wstring& outDir, const std::string& asset, const std::string& exportText,
                        const std::wstring& gdtPath, const std::string& gdtOutputPath, const std::string& collisionLod, bool clean,
                        std::string& error)
{
    std::wstring exp = Join(outDir, Utf8Wide(asset) + L".xmodel_export");
    if (!WriteFileText(exp, exportText))
    {
        error = "Could not write the XModel export.";
        return false;
    }
    FileLog("export written: %s bytes=%zu", WideUtf8(exp).c_str(), exportText.size());

    g_buildPhase.store(PhaseCompile, std::memory_order_release);
    std::wstring bin = Join(outDir, Utf8Wide(asset) + L".xmodel_bin");
    DeleteFileW(bin.c_str());
    std::wstring converter = Join(Join(root, L"bin"), L"export2bin.exe");
    if (Run(converter, L"/v \"" + Utf8Wide(asset) + L".xmodel_export\"", outDir, BuildCancelled, 300000) != 0 ||
        GetFileAttributesW(bin.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        error =
            BuildCancelled() ? "Build cancelled before the GDT was changed." : "export2bin failed; see BrushModeler.log beside the DLL.";
        return false;
    }
    if (BuildCancelled())
    {
        error = "Build cancelled before the GDT was changed.";
        return false;
    }

    g_buildPhase.store(PhaseGdt, std::memory_order_release);
    if (!IsGdtTarget(WideUtf8(gdtPath)))
    {
        error = "Refusing to write: the GDT target must end in .gdt.";
        return false;
    }
    std::string gdt;
    if (!ReadFileText(gdtPath, gdt) || gdt.empty())
    {
        error = "Could not read the GDT file " + WideUtf8(fs::path(gdtPath).filename().wstring()) + ".";
        return false;
    }
    // Drop an older entry of the same xmodel (only possible if the file changed since the name was chosen).
    RemoveGdtEntries(gdt, asset, "xmodel.gdf");
    size_t end = gdt.rfind('}');
    if (end == std::string::npos)
    {
        error = "The GDT file has invalid brace structure.";
        return false;
    }
    std::string filename = gdtOutputPath.empty() ? asset + ".xmodel_bin" : gdtOutputPath + "\\\\" + asset + ".xmodel_bin";
    std::string entry = "\n\t\"" + asset + "\" ( \"xmodel.gdf\" )\n\t{\n\t\t\"BulletCollisionLOD\" \"" + collisionLod +
                        "\"\n\t\t\"filename\" \"" + filename + "\"\n\t\t\"type\" \"rigid\"\n\t}\n";
    gdt.insert(end, entry);
    if (std::string writeError; !WriteGdtAtomically(gdtPath, gdt, writeError))
    {
        error = writeError;
        return false;
    }
    FileLog("GDT entry written: asset=%s collision=%s filename=%s gdt=%s", asset.c_str(), collisionLod.c_str(), filename.c_str(),
            WideUtf8(gdtPath).c_str());
    if (clean && !DeleteFileW(exp.c_str())) FileLog("cleanExports could not remove %s error=%lu", WideUtf8(exp).c_str(), GetLastError());

    // The GDT is changed from here on, so only unloading stops gdtdb; a user cancel lets it finish.
    g_buildPhase.store(PhaseDatabase, std::memory_order_release);
    if (Run(Join(Join(root, L"gdtdb"), L"gdtdb.exe"), L"/update", root, Unloading, 600000) != 0)
    {
        error = "Model built and GDT updated, but gdtdb /update failed. See log.";
        return false;
    }
    return true;
}

// Everything after capture: geometry, merging, export2bin, the GDT entry and gdtdb. Returns the map text to import;
// converted receives the brushes the model replaces.
std::string RunBuild(const BuildJob& job, std::string& error, std::vector<std::string>& converted)
{
    auto cancelled = BuildCancelled;

    g_buildPhase.store(PhaseGeometry, std::memory_order_release);
    std::string text;
    if (!job.brushes.empty())
    {
        ULONGLONG started = GetTickCount64();
        std::vector<Face> faces = BuildFaces(job.brushes, job.skipTools, converted);
        if (faces.empty() && job.models.empty())
        {
            error =
                job.skipTools
                    ? "The selection has no visible faces to convert (only tool faces such as caulk or clip, or none could be rebuilt). "
                      "See BrushModeler.log."
                    : "Could not reconstruct selected brush faces. Check BrushModeler.log for the brush GUID and plane count.";
            return {};
        }
        if (!faces.empty())
        {
            ExportStats stats;
            text = BuildXModelExport(faces, job.pivot, stats);
            FileLog("export source built: brushes=%zu vertices=%zu triangles=%zu materials=%zu took_ms=%llu", job.brushes.size(),
                    stats.vertices, stats.triangles, stats.materials, GetTickCount64() - started);
        }
    }
    bool emptyBase = text.empty();
    if (emptyBase) text = EmptyXModelExport();
    if (cancelled())
    {
        error = "Build cancelled. Nothing was written.";
        return {};
    }

    std::string merged;
    std::unique_ptr<Staging> staging;
    try
    {
        xm::Doc doc = xm::Parse(text, emptyBase);
        xm::Cleanup cleanup;
        if (job.models.empty())
        {
            merged = xm::Write(doc, cleanup);
            FileLog("mesh cleanup: vertices=%zu->%zu faces=%zu->%zu degenerate_removed=%zu exact_duplicates_removed=%zu "
                    "mode=brush_patch_only bounds=%s",
                    cleanup.verticesBefore, cleanup.verticesAfter, cleanup.facesBefore, cleanup.facesAfter, cleanup.degenerateRemoved,
                    cleanup.duplicateRemoved, xm::FormatBounds(cleanup.bounds).c_str());
        }
        else
        {
            g_buildPhase.store(PhaseModels, std::memory_order_release);
            staging = std::make_unique<Staging>(job.outDir, "merge");
            if (!staging->Ok())
            {
                error = "Could not create a staging folder in the output folder.";
                return {};
            }
            xm::Doc combined = xm::Normalized(doc);
            for (size_t i = 0; i < job.models.size(); ++i)
            {
                const auto& m = job.models[i];
                FileLog("model transform dispatch index=%zu guid=%s asset=%s source=%s source_origin=%.6f %.6f %.6f output_origin=%.6f "
                        "%.6f %.6f "
                        "angles_pitch_yaw_roll=%.6f %.6f %.6f scale=%.4f",
                        i, m.guid.c_str(), m.asset.c_str(), WideUtf8(m.source).c_str(), m.origin[0], m.origin[1], m.origin[2], job.pivot.x,
                        job.pivot.y, job.pivot.z, m.angles[0], m.angles[1], m.angles[2], m.scale);
                std::string modelText;
                if (!ModelExportText(m, i, job.pluginDir, staging->Dir(), cancelled, modelText, error)) return {};
                if (cancelled())
                {
                    error = "Build cancelled. Nothing was written.";
                    return {};
                }
                xm::Placement placement;
                for (int k = 0; k < 3; ++k)
                {
                    placement.origin[k] = m.origin[k];
                    placement.angles[k] = m.angles[k];
                }
                placement.center[0] = job.pivot.x;
                placement.center[1] = job.pivot.y;
                placement.center[2] = job.pivot.z;
                placement.scale = m.scale;
                xm::Doc model;
                try
                {
                    model = xm::Parse(modelText);
                }
                catch (const std::exception& e)
                {
                    error = "Model merge failed for " + m.asset + ": " + e.what() + ".";
                    FileLog("merge failed: asset=%s %s", m.asset.c_str(), e.what());
                    return {};
                }
                xm::AppendReport report = xm::Append(combined, model, placement);
                FileLog("model transform audit: asset=%s strategy=%s source_bones=%zu source_vertices=%zu source_faces=%zu "
                        "source_vertex_offset_bounds=%s "
                        "source_root=%s translation=(%.6f,%.6f,%.6f) transformed_vertex_offset_bounds=%s "
                        "transformed_bind_bone_offset_bounds=%s",
                        m.asset.c_str(), report.strategy, report.sourceBones, report.sourceVertices, report.sourceFaces,
                        report.sourceBounds.c_str(), report.sourceRoot.c_str(), placement.origin[0] - placement.center[0],
                        placement.origin[1] - placement.center[1], placement.origin[2] - placement.center[2],
                        report.transformedBounds.c_str(), report.transformedBoneBounds.c_str());
            }
            merged = xm::Write(combined, cleanup);
            FileLog("mesh cleanup: vertices=%zu->%zu faces=%zu->%zu degenerate_removed=%zu exact_duplicates_removed=%zu mode=batch_merge "
                    "models=%zu",
                    cleanup.verticesBefore, cleanup.verticesAfter, cleanup.facesBefore, cleanup.facesAfter, cleanup.degenerateRemoved,
                    cleanup.duplicateRemoved, job.models.size());
            FileLog("combined export bounds: vertices=%zu faces=%zu vertex_offset_bounds=%s", cleanup.verticesAfter, cleanup.facesAfter,
                    xm::FormatBounds(cleanup.bounds).c_str());
        }
    }
    catch (const std::exception& e)
    {
        error = std::string("Building the XModel export failed: ") + e.what() + ". See BrushModeler.log.";
        FileLog("export build failed: %s", e.what());
        return {};
    }
    if (!CompileAndRegister(job.root, job.outDir, job.asset, merged, job.gdtPath, job.gdtOutputPath, job.collisionLod, job.clean, error))
        return {};
    if (staging) staging->Finish(true, job.clean);
    char mapbuf[1024]{};
    snprintf(mapbuf, sizeof mapbuf,
             "{\n\"classname\" \"worldspawn\"\n}\n{\n\"classname\" \"misc_model\"\n\"model\" \"%s\"\n\"origin\" \"%.3f %.3f %.3f\"\n}\n",
             job.asset.c_str(), job.pivot.x, job.pivot.y, job.pivot.z);
    FileLog("background asset build completed: asset=%s merged_models=%zu source_brushes=%zu", job.asset.c_str(), job.models.size(),
            job.brushes.size());
    return mapbuf;
}

// World-space X extent of local bounds turned by the entity's angles.
std::pair<double, double> BoundsWorldX(const xm::Bounds& b, const std::array<float, 3>& angles, double scale)
{
    const double rad = 3.14159265358979323846 / 180.0;
    double pitch = angles[0] * rad, yaw = angles[1] * rad, roll = angles[2] * rad;
    double cp = cos(pitch), sp = sin(pitch), cy = cos(yaw), sy = sin(yaw), cr = cos(roll), sr = sin(roll);
    double r00 = cy * cp, r01 = cy * sp * sr - sy * cr, r02 = cy * sp * cr + sy * sr;
    double lo = 1e300, hi = -1e300;
    for (int ix = 0; ix < 2; ++ix)
        for (int iy = 0; iy < 2; ++iy)
            for (int iz = 0; iz < 2; ++iz)
            {
                double x = ix ? b.hi[0] : b.lo[0], y = iy ? b.hi[1] : b.lo[1], z = iz ? b.hi[2] : b.lo[2];
                double worldX = (r00 * x + r01 * y + r02 * z) * scale;
                lo = (std::min)(lo, worldX);
                hi = (std::max)(hi, worldX);
            }
    return {lo, hi};
}

// Mirrors one queued model into a new asset in its source GDT. Returns the map text placing it beside the original.
std::string RunMirror(const MirrorJob& job, std::string& error)
{
    g_buildPhase.store(PhaseModels, std::memory_order_release);
    Staging staging(job.outDir, "mirror");
    if (!staging.Ok())
    {
        error = "Could not create a staging folder in the output folder.";
        return {};
    }
    std::string text;
    if (!ModelExportText(job.model, 0, job.pluginDir, staging.Dir(), BuildCancelled, text, error)) return {};
    if (BuildCancelled())
    {
        error = "Mirror cancelled. Nothing was written.";
        return {};
    }
    std::string mirrored;
    xm::Bounds source, reflected;
    try
    {
        xm::Doc doc = xm::Parse(text);
        xm::Mirror(doc, job.axes, source, reflected);
        xm::Cleanup cleanup;
        mirrored = xm::Write(doc, cleanup);
        FileLog("mirror export built: asset=%s axes=%c%c%c vertices=%zu->%zu faces=%zu source_bounds=%s mirrored_bounds=%s",
                job.asset.c_str(), (job.axes & 1) ? 'X' : '-', (job.axes & 2) ? 'Y' : '-', (job.axes & 4) ? 'Z' : '-',
                cleanup.verticesBefore, cleanup.verticesAfter, cleanup.facesAfter, xm::FormatBounds(source).c_str(),
                xm::FormatBounds(reflected).c_str());
    }
    catch (const std::exception& e)
    {
        error = std::string("Could not mirror ") + job.model.asset + ": " + e.what() + ".";
        FileLog("mirror failed: %s", e.what());
        return {};
    }
    if (!CompileAndRegister(job.root, job.outDir, job.asset, mirrored, job.model.gdtPath, job.gdtOutputPath, job.collisionLod, job.clean,
                            error))
        return {};
    staging.Finish(true, job.clean);
    // Beside the original along world X, with a 16 unit gap, keeping its rotation and scale.
    const QueuedModel& m = job.model;
    auto originalX = BoundsWorldX(source, m.angles, m.scale), mirrorX = BoundsWorldX(reflected, m.angles, m.scale);
    double placed[3] = {m.origin[0] + originalX.second - mirrorX.first + 16.0, m.origin[1], m.origin[2]};
    char mapBuffer[2048]{};
    int n = snprintf(mapBuffer, sizeof mapBuffer,
                     "{\n\"classname\" \"worldspawn\"\n}\n{\n\"classname\" \"misc_model\"\n\"model\" \"%s\"\n\"origin\" \"%.3f %.3f "
                     "%.3f\"\n\"angles\" \"%.3f "
                     "%.3f %.3f\"\n",
                     job.asset.c_str(), placed[0], placed[1], placed[2], m.angles[0], m.angles[1], m.angles[2]);
    if (m.scale != 1 && n > 0 && n < (int)sizeof mapBuffer)
        snprintf(mapBuffer + n, sizeof mapBuffer - n, "\"modelscale\" \"%g\"\n", m.scale);
    std::string map = std::string(mapBuffer) + "}\n";
    FileLog("mirror completed: source=%s asset=%s axes=%c%c%c source_origin=%.3f %.3f %.3f mirror_origin=%.3f %.3f %.3f source_gdt=%s",
            m.asset.c_str(), job.asset.c_str(), (job.axes & 1) ? 'X' : '-', (job.axes & 2) ? 'Y' : '-', (job.axes & 4) ? 'Z' : '-',
            m.origin[0], m.origin[1], m.origin[2], placed[0], placed[1], placed[2], WideUtf8(m.gdtPath).c_str());
    return map;
}

// ---- replacing the selection (main thread) ---------------------------------------------------------------------

void FinishBuild()
{
    if (!g_buildRunning || !g_buildWorkerDone.load(std::memory_order_acquire)) return;
    if (g_buildThread.joinable()) g_buildThread.join();
    std::string error, map, asset;
    std::vector<std::string> brushGuids, queuedGuids;
    {
        std::lock_guard<std::mutex> lock(g_buildMutex);
        error = g_buildError;
        map = g_buildMap;
        asset = g_buildAsset;
        queuedGuids = g_buildQueuedGuids;
        brushGuids = g_buildBrushGuids;
    }
    g_buildRunning = false;
    g_buildWorkerDone.store(false, std::memory_order_release);
    g_buildCancelRequested.store(false, std::memory_order_relaxed);
    ShowCancel(false);
    g_buildPhase.store(PhaseReplace, std::memory_order_release);
    if (!error.empty())
    {
        Status(error.find("cancelled") != std::string::npos ? Tone::Info : Tone::Error, error);
        return;
    }
    bool undo = rp::Has(&rp::host->EndUndo) && rp::host->BeginUndo && rp::host->EndUndo;
    if (g_buildMode == BuildMode::Mirror)
    {
        Status(Tone::Busy, "Placing the mirrored model beside the original...");
        if (undo) rp::host->BeginUndo(rp::self, "Create mirrored model");
        int imported = rp::Has(&rp::host->ImportMapText) && rp::host->ImportMapText
                           ? rp::host->ImportMapText(rp::self, map.c_str(), "Create mirrored model")
                           : 0;
        std::vector<std::string> importedGuids = imported ? rp::SelectedEntityGuids() : std::vector<std::string>();
        if (undo) rp::host->EndUndo(rp::self);
        if (!imported)
        {
            Status(Tone::Error, "Mirrored asset " + asset + " was added to its GDT, but Radiant could not place it in the map.");
            Log("mirror map placement failed asset=%s", asset.c_str());
            return;
        }
        if (!importedGuids.empty() && rp::Has(&rp::host->SelectEntityByGuid) && rp::host->SelectEntityByGuid)
        {
            rp::host->DeselectAll(rp::self);
            rp::host->SelectEntityByGuid(rp::self, importedGuids.back().c_str());
        }
        if (rp::Has(&rp::host->MarkMapModified) && rp::host->MarkMapModified) rp::host->MarkMapModified(rp::self);
        ForceRadiantAssetRefresh();
        if (rp::Has(&rp::host->RequestRedraw) && rp::host->RequestRedraw) rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_3D);
        Status(Tone::Ready,
               "Mirrored model placed beside the original: " + asset + ". The original is unchanged; use Undo to remove the copy.");
        g_waitingRadiantRefresh = true;
        g_refreshReadyAt = GetTickCount64() + 250;
        g_phaseShown = -1;
        Log("mirror asset placed beside source asset=%s", asset.c_str());
        return;
    }
    std::vector<ModToolsBrush> brushes;
    if (!brushGuids.empty())
    {
        brushes = rp::FindBrushesByGuid(brushGuids);
        if (brushes.size() != brushGuids.size() || std::any_of(brushes.begin(), brushes.end(), [](ModToolsBrush b) { return !b; }))
        {
            Status(Tone::Error,
                   "XModel built, but Radiant selection changed maps before replacement; original brushes were left in place.");
            Log("map replacement skipped: source brushes no longer exist");
            return;
        }
    }
    Status(Tone::Busy, "Radiant is adding the model and replacing the selected geometry...");
    if (undo) rp::host->BeginUndo(rp::self, "Build combined XModel");
    int imported = rp::Has(&rp::host->ImportMapText) && rp::host->ImportMapText
                       ? rp::host->ImportMapText(rp::self, map.c_str(), "Build combined XModel")
                       : 0;
    if (!imported)
    {
        if (undo) rp::host->EndUndo(rp::self);
        Status(Tone::Error, "Model asset built, but Radiant could not add it; the selected objects were left in place.");
        return;
    }
    if (!brushes.empty())
    {
        int deleted = rp::Has(&rp::host->DeleteBrushes) && rp::host->DeleteBrushes
                          ? rp::host->DeleteBrushes(rp::self, brushes.data(), brushes.size(), "Build combined XModel")
                          : 0;
        if (!deleted)
        {
            if (undo) rp::host->EndUndo(rp::self);
            if (rp::Has(&rp::host->TriggerMenuAction) && rp::host->TriggerMenuAction) rp::host->TriggerMenuAction(rp::self, "Edit/Undo");
            Status(Tone::Error, "Model asset built, but brush replacement failed. Radiant undo was applied; see log.");
            Log("replacement failed: brush deletion returned false");
            return;
        }
    }
    std::vector<std::string> importedGuids = rp::SelectedEntityGuids();
    std::string outputGuid = importedGuids.empty() ? std::string() : importedGuids.back();
    if (!queuedGuids.empty() && rp::Has(&rp::host->SelectEntityByGuid) && rp::host->SelectEntityByGuid &&
        rp::Has(&rp::host->TriggerMenuAction) && rp::host->TriggerMenuAction)
    {
        for (const auto& guid : queuedGuids)
        {
            rp::host->DeselectAll(rp::self);
            if (rp::host->SelectEntityByGuid(rp::self, guid.c_str()))
            {
                int del = rp::host->TriggerMenuAction(rp::self, "Edit/Delete");
                Log("source model entity removed after merge guid=%s command_result=%d", guid.c_str(), del);
            }
            else
                Log("source model entity removal skipped: guid no longer exists=%s", guid.c_str());
        }
    }
    if (undo) rp::host->EndUndo(rp::self);
    if (!outputGuid.empty() && rp::Has(&rp::host->SelectEntityByGuid) && rp::host->SelectEntityByGuid)
    {
        rp::host->DeselectAll(rp::self);
        rp::host->SelectEntityByGuid(rp::self, outputGuid.c_str());
    }
    if (rp::Has(&rp::host->MarkMapModified) && rp::host->MarkMapModified) rp::host->MarkMapModified(rp::self);
    bool cameraRefresh = ForceRadiantAssetRefresh();
    if (rp::Has(&rp::host->RequestRedraw) && rp::host->RequestRedraw) rp::host->RequestRedraw(rp::self, MODTOOLS_VIEW_3D);
    Status(Tone::Ready, cameraRefresh ? "Converted. Refreshed the Radiant 3D view."
                                      : "Converted. Requested a 3D redraw; asset refresh was unavailable.");
    g_waitingRadiantRefresh = true;
    g_refreshReadyAt = GetTickCount64() + 250;
    g_phaseShown = -1;
    Log("conversion success asset=%s brush_count=%zu merged_model_count=%zu", asset.c_str(), brushes.size(), queuedGuids.size());
    std::string nextAsset = NextAssetName(asset);
    if (g_asset && rp::host->ui) rp::host->ui->SetEditText(g_asset, nextAsset.c_str());
    rp::ConfigSet("model", "asset", nextAsset);
    Log("next model asset name set to %s", nextAsset.c_str());
    g_queuedModels.clear();
    ModelStatus("Model merge: select models, queue them, wait for lookup, then build. Brushes/patches can be built directly.");
}

// The GDT "filename" folder: the output folder relative to model_export, backslashes doubled. False across drives.
// inside is set when the folder is model_export itself or below it.
bool GdtFolder(const fs::path& absOut, const fs::path& modelExportRoot, std::string& folder, bool* inside = nullptr)
{
    // Windows paths ignore case, so compare lowercase copies and keep the folder's own spelling where possible.
    auto lower = [](std::wstring s) {
        if (!s.empty()) CharLowerBuffW(&s[0], (DWORD)s.size());
        return fs::path(s);
    };
    fs::path out = absOut.lexically_normal();
    fs::path rel = lower(out.wstring()).lexically_relative(lower(modelExportRoot.lexically_normal().wstring()));
    if (rel.empty()) return false;
    bool below = *rel.begin() != L"..";
    if (inside) *inside = below;
    if (below && rel != L".")
    {
        size_t depth = (size_t)std::distance(rel.begin(), rel.end());
        std::vector<fs::path> parts(out.begin(), out.end());
        while (!parts.empty() && parts.back().empty()) parts.pop_back(); // a trailing separator
        if (parts.size() >= depth)
        {
            fs::path original;
            for (size_t i = parts.size() - depth; i < parts.size(); ++i) original /= parts[i];
            rel = original;
        }
    }
    std::string relative = WideUtf8(rel.wstring());
    std::replace(relative.begin(), relative.end(), '/', '\\');
    if (relative == ".") relative.clear();
    folder.clear();
    for (char c : relative)
    {
        folder.push_back(c);
        if (c == '\\') folder.push_back('\\');
    }
    return true;
}

// The output folder from the pane, made absolute and created. False (with the status set) when it cannot be used.
bool PrepareOutputDir(const std::wstring& root, bool requireInside, std::wstring& outDir, std::string& gdtOutputPath)
{
    std::string outputValue = Edit(g_outputDir);
    outDir = outputValue.empty() ? DefaultOutputDir() : Utf8Wide(outputValue);
    std::error_code pathError;
    fs::path absOut = fs::absolute(fs::path(outDir), pathError).lexically_normal();
    if (outDir.empty() || pathError)
    {
        Status(Tone::Error, "Could not resolve the selected output folder.");
        return false;
    }
    outDir = absOut.wstring();
    bool inside = false;
    if (!GdtFolder(absOut, fs::path(root) / L"model_export", gdtOutputPath, &inside))
    {
        Status(Tone::Error, "The output folder must be on the same drive as the BO3 model_export folder so the GDT can reference it.");
        return false;
    }
    if (requireInside && !inside)
    {
        Status(Tone::Error, "The output folder must be inside BO3's model_export folder so the GDT can reference the mirrored model.");
        return false;
    }
    std::error_code createError;
    fs::create_directories(absOut, createError);
    if (createError)
    {
        Status(Tone::Error, "Could not create the selected output folder.");
        Log("output directory creation failed path=%s error=%d", WideUtf8(outDir).c_str(), createError.value());
        return false;
    }
    rp::ConfigSet("model", "output_dir", WideUtf8(outDir));
    return true;
}

void StartWorker(std::function<void(std::string& error, std::string& map, std::vector<std::string>& converted)> work)
{
    g_buildCancelRequested.store(false, std::memory_order_relaxed);
    g_buildWorkerDone.store(false, std::memory_order_release);
    g_buildStartedAt = GetTickCount64();
    g_bannerUpdatedAt = 0;
    g_phaseShown = -1;
    g_buildRunning = true;
    ShowCancel(true);
    g_buildThread = std::thread([work = std::move(work)]() {
        std::string error, map;
        std::vector<std::string> converted;
        try
        {
            work(error, map, converted);
        }
        catch (const std::exception& e)
        {
            error = std::string("Unexpected error while building: ") + e.what() + ". See BrushModeler.log.";
            FileLog("unhandled exception in build worker: %s", e.what());
        }
        catch (...)
        {
            error = "Unexpected error while building. See BrushModeler.log.";
            FileLog("unhandled exception in build worker");
        }
        {
            std::lock_guard<std::mutex> lock(g_buildMutex);
            g_buildError = error;
            g_buildMap = map;
            g_buildBrushGuids = std::move(converted);
        }
        g_buildWorkerDone.store(true, std::memory_order_release);
    });
}

void OnMirrorModel(void*)
{
    try
    {
        if (g_lookupRunning)
        {
            Status(Tone::Warning, "Wait for the model source search to finish before mirroring.");
            return;
        }
        if (g_buildRunning)
        {
            Status(Tone::Warning, "Wait for the current model build to finish before mirroring.");
            return;
        }
        if (g_buildThread.joinable()) g_buildThread.join();
        if (g_queuedModels.size() != 1)
        {
            Status(Tone::Warning, "Select one Radiant model, queue it, and wait for its source search before mirroring.");
            return;
        }
        const QueuedModel& model = g_queuedModels.front();
        if (model.source.empty() || model.gdtPath.empty())
        {
            Status(Tone::Error, "The queued model is missing its source file or GDT path. Queue it again.");
            return;
        }
        unsigned axes = 0;
        if (g_mirrorX && rp::host->ui->IsChecked(g_mirrorX)) axes |= 1;
        if (g_mirrorY && rp::host->ui->IsChecked(g_mirrorY)) axes |= 2;
        if (g_mirrorZ && rp::host->ui->IsChecked(g_mirrorZ)) axes |= 4;
        if (!axes)
        {
            Status(Tone::Warning, "Choose at least one axis to mirror across.");
            return;
        }
        auto job = std::make_shared<MirrorJob>();
        job->root = GameRoot();
        if (!PrepareOutputDir(job->root, true, job->outDir, job->gdtOutputPath)) return;
        job->pluginDir = rp::PluginDir();
        job->asset = UniqueAssetName(model.asset + "_mirror", model.gdtPath, job->root);
        job->model = model;
        job->axes = axes;
        job->clean = CleanExports();
        job->collisionLod = ComboValue(g_collision, "collision", CollisionHigh) == CollisionNone ? "None" : "High";
        SaveOptions();
        g_buildMode = BuildMode::Mirror;
        g_buildPhase.store(PhaseModels, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lock(g_buildMutex);
            g_buildError.clear();
            g_buildMap.clear();
            g_buildAsset = job->asset;
            g_buildQueuedGuids.clear();
            g_buildBrushGuids.clear();
        }
        Log("mirror requested: source=%s asset=%s axes=%u", model.asset.c_str(), job->asset.c_str(), axes);
        StartWorker([job](std::string& error, std::string& map, std::vector<std::string>&) { map = RunMirror(*job, error); });
        Status(Tone::Busy, "Mirroring the selected XModel and adding it to its source GDT...");
    }
    catch (...)
    {
        Status(Tone::Error, "Could not start the mirror operation. See BrushModeler.log.");
        Log("exception while starting mirror operation");
    }
}

void OnConvert(void*)
{
    try
    {
        if (g_lookupRunning)
        {
            Status(Tone::Warning, "Wait for the selected model lookup to finish before starting a build.");
            return;
        }
        if (g_buildRunning)
        {
            Status(Tone::Info, "A model build is already running.");
            return;
        }
        if (g_buildThread.joinable()) g_buildThread.join();
        auto rawSelection = rp::GetSelection();
        auto unqueuedModels = UnqueuedSelectedModels(rawSelection);
        if (!unqueuedModels.empty())
        {
            for (const auto& m : unqueuedModels)
                Log("build blocked: selected model was not queued guid=%s asset=%s", m.guid.c_str(), m.asset.c_str());
            Status(Tone::Warning, "Selected model(s) need to be queued first. Click Queue selected models for merge, wait for the search "
                                  "to finish, then build.");
            return;
        }
        std::vector<ModToolsSelectedBrush> s;
        std::vector<std::string> guids;
        size_t skippedQueuedModel = 0;
        for (auto& item : rawSelection)
            if (item.brushGuid[0])
            {
                if (item.entityGuid[0] && IsQueued(item.entityGuid))
                {
                    ++skippedQueuedModel;
                    Log("queued model selection proxy excluded from brush capture: guid=%s brush_guid=%s", item.entityGuid, item.brushGuid);
                    continue;
                }
                s.push_back(item);
                guids.emplace_back(item.brushGuid);
            }
        Log("build requested selected_items=%zu brush_items=%zu skipped_queued_model_proxies=%zu queued_models=%zu", rawSelection.size(),
            s.size(), skippedQueuedModel, g_queuedModels.size());
        if (s.empty() && g_queuedModels.empty())
        {
            Status(Tone::Warning, "Select brushes or patches, or queue one or more selected models first.");
            return;
        }
        std::string asset = Edit(g_asset), gdtText = Edit(g_gdt);
        if (asset.empty() || gdtText.empty())
        {
            Status(Tone::Warning, "Enter an asset name and choose a GDT file.");
            return;
        }
        if (!IsGdtTarget(gdtText))
        {
            Status(Tone::Error, "Choose a .gdt file. Map files are blocked as GDT targets.");
            Log("refused build because GDT target is not .gdt: %s", gdtText.c_str());
            return;
        }
        Log("GDT update target selected: %s", gdtText.c_str());
        for (char& c : asset)
            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_')) c = '_';
        std::wstring root = GameRoot();
        asset = UniqueAssetName(asset, Utf8Wide(gdtText), root);
        auto foundBrushes = rp::FindBrushesByGuid(guids);
        std::vector<ModToolsSelectedBrush> validSelection;
        std::vector<ModToolsBrush> brushes;
        for (size_t i = 0; i < foundBrushes.size() && i < s.size(); ++i)
            if (foundBrushes[i])
            {
                validSelection.push_back(s[i]);
                brushes.push_back(foundBrushes[i]);
            }
        s.swap(validSelection);
        if (!s.empty() && s.size() != guids.size())
            Log("some selected geometry items could not be resolved: resolved=%zu requested=%zu", s.size(), guids.size());
        if (s.empty() && g_queuedModels.empty())
        {
            Status(Tone::Error, "The current selection has no convertible brushes. Check BrushModeler.log.");
            return;
        }

        int pivotMode = ComboValue(g_pivot, "pivot", PivotCenter);
        Vec pivot{};
        if (!s.empty())
        {
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (const auto& b : s)
                for (int k = 0; k < 3; ++k)
                {
                    lo[k] = (std::min)(lo[k], b.mins[k]);
                    hi[k] = (std::max)(hi[k], b.maxs[k]);
                }
            pivot = {(lo[0] + hi[0]) * .5f, (lo[1] + hi[1]) * .5f, pivotMode == PivotBottomCenter ? lo[2] : (lo[2] + hi[2]) * .5f};
        }
        else
        {
            // Models only: their origins are all that is known before the merge, so both modes use their average.
            for (const auto& m : g_queuedModels)
            {
                pivot.x += m.origin[0];
                pivot.y += m.origin[1];
                pivot.z += m.origin[2];
            }
            pivot = Mul(pivot, 1.0f / (float)g_queuedModels.size());
        }

        std::wstring outDir;
        std::string gdtOutputPath;
        if (!PrepareOutputDir(root, false, outDir, gdtOutputPath)) return;
        rp::ConfigSet("model", "asset", asset);
        SaveOptions();
        Log("model output folder: %s gdt_relative=%s", WideUtf8(outDir).c_str(), gdtOutputPath.c_str());

        auto job = std::make_shared<BuildJob>();
        job->root = root;
        job->outDir = outDir;
        job->pluginDir = rp::PluginDir();
        job->gdtPath = Utf8Wide(gdtText);
        job->asset = asset;
        job->gdtOutputPath = gdtOutputPath;
        job->collisionLod = ComboValue(g_collision, "collision", CollisionHigh) == CollisionNone ? "None" : "High";
        job->models = g_queuedModels;
        job->pivot = pivot;
        job->skipTools = SkipToolFaces();
        job->clean = CleanExports();
        g_buildMode = BuildMode::Combined;
        Log("combined output placement anchor origin=%.6f %.6f %.6f queued_models=%zu selected_geometry=%zu cleanExports=%d", pivot.x,
            pivot.y, pivot.z, g_queuedModels.size(), s.size(), job->clean ? 1 : 0);
        g_buildPhase.store(PhaseGeometry, std::memory_order_release);
        if (!s.empty())
        {
            Status(Tone::Busy, "Reading selected brush and patch geometry...");
            ULONGLONG captureStart = GetTickCount64();
            std::string err;
            if (!CaptureSelectionProtected(s, brushes, job->brushes, err))
            {
                Status(Tone::Error, err);
                return;
            }
            Log("geometry capture took_ms=%llu brushes=%zu", (unsigned long long)(GetTickCount64() - captureStart), job->brushes.size());
        }
        {
            std::lock_guard<std::mutex> lock(g_buildMutex);
            g_buildError.clear();
            g_buildMap.clear();
            g_buildAsset = asset;
            g_buildQueuedGuids.clear();
            for (const auto& m : g_queuedModels) g_buildQueuedGuids.push_back(m.guid);
            g_buildBrushGuids.clear();
        }
        StartWorker(
            [job](std::string& error, std::string& map, std::vector<std::string>& converted) { map = RunBuild(*job, error, converted); });
        Status(Tone::Busy, "Building the combined XModel in the background... Radiant remains responsive.");
    }
    catch (...)
    {
        Status(Tone::Error, "Unexpected error; see BrushModeler.log beside the DLL.");
        Log("unhandled exception in Convert");
    }
}

// ---- plugin lifecycle ------------------------------------------------------------------------------------------

void OnUnload()
{
    g_unloading.store(true, std::memory_order_relaxed);
    g_lookupCancelRequested.store(true, std::memory_order_relaxed);
    g_buildCancelRequested.store(true, std::memory_order_relaxed);
    if (g_lookupThread.joinable()) g_lookupThread.join();
    if (g_buildThread.joinable()) g_buildThread.join();
}

ModToolsWidget FieldLabel(const ModToolsUiApi* u, const char* text, ModToolsWidget root)
{
    auto label = u->Label(text, root);
    u->SetMinimumSize(label, 56, 20);
    u->SetMaximumWidth(label, 56);
    return label;
}
void OnComboChanged(void*, int) { SaveOptions(); }
void OnSkipToolsClicked(void*) { SaveOptions(); }

int OnLoad()
{
    if (!rp::InRadiant()) return 1;
    std::wstring dll = rp::PluginDir();
    g_log = Join(dll, L"BrushModeler.log");
    Log("=== BrushModeler " BRUSHMODELER_VERSION " OnLoad ===");
    if (!rp::host->ui) return 1;
    const auto* u = rp::host->ui;
    auto root = u->Widget(nullptr);
    auto box = u->VBox(root);
    u->LayoutMargins(box, 7, 6, 7, 6);
    u->LayoutSpacing(box, 5);
    auto title = u->Label("<b>Brush Modeler</b> <small>Brushes, patches and model merging</small>", root);
    u->SetMaximumHeight(title, 26);
    u->BoxAddWidget(box, title, 0);
    g_status = u->Label("", root);
    u->SetWordWrap(g_status, 1);
    u->SetMaximumHeight(g_status, 54);
    auto statusRow = u->HBox();
    u->LayoutSpacing(statusRow, 5);
    u->BoxAddWidget(statusRow, g_status, 1);
    g_cancel = u->PushButton("Cancel", root);
    u->SetToolTip(g_cancel, "Stop the running model search or build.");
    u->SetVisible(g_cancel, 0);
    u->BoxAddWidget(statusRow, g_cancel, 0);
    u->BoxAddLayout(box, statusRow, 0);

    auto addRow = [&](const char* label, ModToolsWidget field, ModToolsWidget button) {
        auto row = u->HBox();
        u->LayoutSpacing(row, 6);
        u->BoxAddWidget(row, FieldLabel(u, label, root), 0);
        u->BoxAddWidget(row, field, 1);
        if (!button)
        {
            button = u->Label("", root);
            u->SetMinimumSize(button, 68, 24);
        }
        else
            u->SetMinimumSize(button, 68, 24);
        u->SetMaximumWidth(button, 68);
        u->BoxAddWidget(row, button, 0);
        u->BoxAddLayout(box, row, 0);
    };
    g_asset = u->LineEdit(rp::ConfigGet("model", "asset", "combined_selection").c_str(), root);
    u->SetPlaceholder(g_asset, "Combined model name");
    u->SetToolTip(g_asset, "XModel asset name. A name already used in the GDT or gdt.db gets a numeric suffix.");
    addRow("Asset", g_asset, nullptr);
    g_gdt = u->LineEdit(rp::ConfigGet("model", "gdt", "").c_str(), root);
    u->SetPlaceholder(g_gdt, "GDT file to update");
    u->SetToolTip(g_gdt, "Choose the GDT that will receive the generated XModel entry.");
    auto browse = u->PushButton("Browse…", root);
    u->SetToolTip(browse, "Choose a GDT file.");
    addRow("GDT", g_gdt, browse);
    std::string outputDefault = rp::ConfigGet("model", "output_dir", "");
    if (outputDefault.empty()) outputDefault = WideUtf8(DefaultOutputDir());
    g_outputDir = u->LineEdit(outputDefault.c_str(), root);
    u->SetPlaceholder(g_outputDir, "Model output folder");
    u->SetToolTip(g_outputDir, "Folder for generated .xmodel_export and .xmodel_bin files.");
    auto outputBrowse = u->PushButton("Browse…", root);
    u->SetToolTip(outputBrowse, "Choose an output folder.");
    addRow("Output", g_outputDir, outputBrowse);

    if (rp::HasUi(&u->ComboSetCurrent) && u->ComboBox)
    {
        auto row = u->HBox();
        u->LayoutSpacing(row, 6);
        u->BoxAddWidget(row, FieldLabel(u, "Collision", root), 0);
        g_collision = u->ComboBox(root);
        u->ComboAdd(g_collision, "None");
        u->ComboAdd(g_collision, "Model (LOD0)");
        u->ComboSetCurrent(g_collision, rp::ConfigGetInt("model", "collision", CollisionHigh) == CollisionNone ? 0 : 1);
        u->SetToolTip(g_collision,
                      "Bullet collision for the new model (the GDT's BulletCollisionLOD). The original brushes are removed, so "
                      "without it nothing blocks shots where they were.");
        u->BoxAddWidget(row, g_collision, 1);
        auto pivotLabel = u->Label("Pivot", root);
        u->BoxAddWidget(row, pivotLabel, 0);
        g_pivot = u->ComboBox(root);
        u->ComboAdd(g_pivot, "Center");
        u->ComboAdd(g_pivot, "Bottom center");
        u->ComboSetCurrent(g_pivot, rp::ConfigGetInt("model", "pivot", PivotCenter) == PivotBottomCenter ? 1 : 0);
        u->SetToolTip(g_pivot,
                      "Model origin: the middle of the selected brushes' bounds, or the middle of their base (easier to place on a "
                      "floor). Models-only builds use the models' average origin.");
        u->BoxAddWidget(row, g_pivot, 1);
        u->BoxAddLayout(box, row, 0);
        if (rp::Has(&rp::host->OnChanged) && rp::host->OnChanged)
        {
            rp::host->OnChanged(rp::self, g_collision, OnComboChanged, nullptr);
            rp::host->OnChanged(rp::self, g_pivot, OnComboChanged, nullptr);
        }
    }
    if (u->CheckBox && u->SetChecked)
    {
        g_skipTools = u->CheckBox("Skip tool faces (caulk, clip, nodraw, portal, trigger…)", root);
        u->SetChecked(g_skipTools, rp::ConfigGetBool("model", "skip_tool_faces", true) ? 1 : 0);
        u->SetToolTip(g_skipTools,
                      "Leave faces with Radiant tool materials out of the model. They never render, so exporting them only adds "
                      "triangles.");
        u->BoxAddWidget(box, g_skipTools, 0);
        rp::host->OnClicked(rp::self, g_skipTools, OnSkipToolsClicked, nullptr);
    }

    auto actionRow = u->HBox();
    u->LayoutSpacing(actionRow, 5);
    auto queue = u->PushButton("Queue selected models for merge", root);
    u->SetToolTip(queue,
                  "First select the Radiant model entities, then queue them here. Wait for the source search to finish before building or "
                  "mirroring.");
    u->BoxAddWidget(actionRow, queue, 1);
    auto convert = u->PushButton("Build combined model", root);
    u->SetToolTip(convert, "Build from selected brushes/patches and any models queued for merge. Selected models must be queued first.");
    u->BoxAddWidget(actionRow, convert, 1);
    u->BoxAddLayout(box, actionRow, 0);
    ModToolsWidget mirror = nullptr;
    if (u->CheckBox && u->SetChecked && u->IsChecked)
    {
        auto mirrorRow = u->HBox();
        u->LayoutSpacing(mirrorRow, 5);
        u->BoxAddWidget(mirrorRow, FieldLabel(u, "Mirror", root), 0);
        g_mirrorX = u->CheckBox("X", root);
        g_mirrorY = u->CheckBox("Y", root);
        g_mirrorZ = u->CheckBox("Z", root);
        u->SetChecked(g_mirrorX, 1);
        u->SetToolTip(g_mirrorX, "Reflect the model's local X coordinates.");
        u->SetToolTip(g_mirrorY, "Reflect the model's local Y coordinates.");
        u->SetToolTip(g_mirrorZ, "Reflect the model's local Z coordinates.");
        u->BoxAddWidget(mirrorRow, g_mirrorX, 0);
        u->BoxAddWidget(mirrorRow, g_mirrorY, 0);
        u->BoxAddWidget(mirrorRow, g_mirrorZ, 0);
        mirror = u->PushButton("Create mirrored model", root);
        u->SetToolTip(mirror,
                      "Create a mirrored XModel in the selected model's source GDT and place a copy beside the original. Queue exactly one "
                      "model first.");
        u->BoxAddWidget(mirrorRow, mirror, 1);
        u->BoxAddLayout(box, mirrorRow, 0);
    }
    g_modelStatus =
        u->Label("Select a model, queue it, and wait for lookup. Build combined models or create a mirrored copy in the source GDT.", root);
    u->SetWordWrap(g_modelStatus, 1);
    u->SetMaximumHeight(g_modelStatus, 40);
    u->BoxAddWidget(box, g_modelStatus, 0);
    g_count = u->Label("Selection: checking…", root);
    u->SetWordWrap(g_count, 1);
    u->SetMaximumHeight(g_count, 28);
    u->BoxAddWidget(box, g_count, 0);
    Status(Tone::Ready, "Ready: build selected brushes/patches directly, or select models and queue them for merge before building.");
    auto help = u->Label(
        "Output: .xmodel_bin (+ .xmodel_export when cleanExports=false). Brush replacement and mirrored copies can be undone.", root);
    u->SetWordWrap(help, 1);
    u->SetMaximumHeight(help, 36);
    u->BoxAddWidget(box, help, 0);
    u->BoxAddStretch(box, 1);
    rp::host->OnClicked(rp::self, browse, OnBrowseGdt, nullptr);
    rp::host->OnClicked(rp::self, outputBrowse, OnBrowseOutput, nullptr);
    rp::host->OnClicked(rp::self, queue, OnQueueModel, nullptr);
    rp::host->OnClicked(rp::self, g_cancel, OnCancel, nullptr);
    rp::host->OnClicked(rp::self, convert, OnConvert, nullptr);
    if (mirror) rp::host->OnClicked(rp::self, mirror, OnMirrorModel, nullptr);
    g_pane = rp::host->CreatePane(rp::self, "Brush to XModel", root, 390, 390);
    if (g_pane && rp::Has(&rp::host->TabPaneWith) && rp::host->TabPaneWith)
    {
        int docked = rp::host->TabPaneWith(rp::self, g_pane, "right:Camera");
        Log("pane right-of-camera placement result=%d", docked);
    }
    auto menu = rp::host->AddMenu(rp::self, "Brush Modeler");
    if (menu && g_pane) rp::host->AddMenuAction(rp::self, menu, rp::host->PaneToggleAction(rp::self, g_pane));
    rp::host->ShowPane(rp::self, g_pane, 1);
    Log("pane created=%d log=%s", g_pane != nullptr, WideUtf8(g_log).c_str());
    return 1;
}

void UpdateSelectionStatus()
{
    static uint64_t lastHash = 0;
    auto s = rp::GetSelection();
    uint64_t hash = 1469598103934665603ull;
    size_t brushCount = 0;
    auto mix = [&](const char* text, unsigned char end) {
        for (const unsigned char* c = (const unsigned char*)text; *c; ++c)
        {
            hash ^= *c;
            hash *= 1099511628211ull;
        }
        hash ^= end;
        hash *= 1099511628211ull;
    };
    for (const auto& b : s)
    {
        mix(b.brushGuid, 0xff);
        mix(b.entityGuid, 0xfe);
        if (b.brushGuid[0] && !IsQueued(b.entityGuid)) ++brushCount;
    }
    if (hash == lastHash) return;
    lastHash = hash;
    char b[128];
    snprintf(b, sizeof b, "Selection: %zu item(s), %zu brush/patch item(s)", s.size(), brushCount);
    if (g_count && rp::host->ui) rp::host->ui->SetLabelText(g_count, b);
    Log("selection changed: items=%zu brushes_and_patches=%zu signature=%016llx", s.size(), brushCount, (unsigned long long)hash);
    if (g_buildRunning || g_lookupRunning || g_waitingRadiantRefresh) return;
    if (!UnqueuedSelectedModels(s).empty())
        Status(Tone::Warning, "Model selected: queue it for merge first, wait for the source search, then build.");
    else if (brushCount)
    {
        char ready[128];
        snprintf(ready, sizeof ready, "Ready to convert: %zu brush/patch item(s) selected.", brushCount);
        Status(Tone::Ready, ready);
    }
    else if (!g_queuedModels.empty())
        Status(Tone::Ready, "Ready to combine " + std::to_string(g_queuedModels.size()) + " queued model(s).");
    else
        Status(Tone::Ready, "Ready: build brushes/patches directly, or queue selected models for merge before building.");
}

void OnTick()
{
    if (g_lookupRunning)
    {
        if (g_lookupDone.load(std::memory_order_acquire))
            FinishModelLookup();
        else if (GetTickCount64() - g_lookupBannerAt >= 1000)
        {
            unsigned long sec = (unsigned long)((GetTickCount64() - g_lookupStartedAt) / 1000);
            char lookupBanner[192];
            snprintf(lookupBanner, sizeof lookupBanner, "%s model search… (%lu:%02lu elapsed)",
                     g_lookupCancelRequested.load(std::memory_order_relaxed) ? "Cancelling" : "Searching GDTs:", sec / 60, sec % 60);
            Status(Tone::Busy, lookupBanner, false);
            g_lookupBannerAt = GetTickCount64();
        }
    }
    if (g_buildRunning)
    {
        int phase = g_buildPhase.load(std::memory_order_acquire);
        ULONGLONG tickNow = GetTickCount64();
        if (phase != g_phaseShown || tickNow - g_bannerUpdatedAt >= 1000)
        {
            const char* msg = "Building the XModel";
            switch (phase)
            {
            case PhaseGeometry:
                msg = "Converting selected brush and patch geometry";
                break;
            case PhaseModels:
                msg = g_buildMode == BuildMode::Mirror ? "Preparing the selected model for mirroring" : "Merging the queued models";
                break;
            case PhaseCompile:
                msg = g_buildMode == BuildMode::Mirror ? "Compiling the mirrored XModel" : "Compiling the XModel";
                break;
            case PhaseGdt:
                msg = g_buildMode == BuildMode::Mirror ? "Registering the mirrored model in its source GDT"
                                                       : "Registering the model in its GDT";
                break;
            case PhaseDatabase:
                msg = "Updating the Radiant asset database";
                break;
            case PhaseReplace:
                msg = "Radiant is adding the model and replacing selected geometry";
                break;
            }
            unsigned long sec = g_buildStartedAt ? (unsigned long)((tickNow - g_buildStartedAt) / 1000) : 0;
            char banner[256];
            snprintf(banner, sizeof banner, "%s%s… (%lu:%02lu elapsed)",
                     g_buildCancelRequested.load(std::memory_order_relaxed) ? "Cancelling: " : "", msg, sec / 60, sec % 60);
            Status(Tone::Busy, banner, false);
            g_phaseShown = phase;
            g_bannerUpdatedAt = tickNow;
        }
        FinishBuild();
        if (g_buildRunning) return;
    }
    ULONGLONG now = GetTickCount64();
    if (g_waitingRadiantRefresh)
    {
        if (now < g_refreshReadyAt) return;
        g_waitingRadiantRefresh = false;
        g_phaseShown = -1;
    }
    static ULONGLONG polled = 0;
    if (now - polled < 1000) return;
    polled = now;
    if (g_pane && rp::host->PaneVisible && !rp::host->PaneVisible(rp::self, g_pane)) return;
    if (rp::host->IsReloading && rp::host->IsReloading(rp::self)) return;
    UpdateSelectionStatus();
}
void OnSettings()
{
    if (g_pane) rp::host->ShowPane(rp::self, g_pane, 1);
}
} // namespace

extern "C" MODTOOLS_PLUGIN_EXPORT const ModToolsPluginApi* ModToolsPluginEntry(ModToolsPlugin* self, const ModToolsHostApi* host)
{
    rp::Bind(self, host);
    static ModToolsPluginApi a{};
    a.size = sizeof a;
    a.apiVersion = MODTOOLS_PLUGIN_API_VERSION;
    a.name = "BrushModeler";
    a.displayName = "Brush Modeler";
    a.version = BRUSHMODELER_VERSION;
    a.author = "Owen";
    a.description = "Convert Radiant brushes, merge BO3 models, and create mirrored model assets.";
    a.apps = MODTOOLS_APP_RADIANT;
    a.OnLoad = OnLoad;
    a.OnUnload = OnUnload;
    a.OnTick = OnTick;
    a.OnSettings = OnSettings;
    return &a;
}
