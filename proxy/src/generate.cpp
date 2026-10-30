// Keeps legacygpu_shaders in step with the installed game. The release ships no converted shaders:
// on the first start, and whenever the game's shader pak or the bundled converters change, the
// proxy runs legacygpu_tools\build_shaders.py with the bundled Python behind a progress window and
// waits for it. build_shaders.py keeps outputs it already has and deletes ones the game dropped.
// stamp.txt in the shader folder records the inputs of the last successful run.
#include <windows.h>
#include <unknwn.h>  // IUnknown, for common.h
#include <commctrl.h>
#include <cstdio>
#include <string>
#include <thread>
#include "common.h"

static const wchar_t* const kTitle = L"Legacy GPU Compatibility";

static bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Size and write time of the files the shader library is built from.
static std::string Fingerprint(const std::wstring* files, int count) {
    std::string s = "1";
    for (int i = 0; i < count; ++i) {
        WIN32_FILE_ATTRIBUTE_DATA a = {};
        GetFileAttributesExW(files[i].c_str(), GetFileExInfoStandard, &a);
        char item[64];
        sprintf_s(item, " %lu:%lu:%lu:%lu", a.nFileSizeHigh, a.nFileSizeLow, a.ftLastWriteTime.dwHighDateTime,
                  a.ftLastWriteTime.dwLowDateTime);
        s += item;
    }
    return s;
}

static std::string ReadText(const std::wstring& path) {
    std::string s;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb")) return s;
    char buf[256];
    for (size_t n; (n = fread(buf, 1, sizeof(buf), f)) > 0;) s.append(buf, n);
    fclose(f);
    return s;
}

// A small always-on-top window with a progress bar while the game waits. It stays hidden for the
// first seconds, so a run with nothing to convert (about 4 s) finishes without it.
struct Progress {
    static const UINT kShowAfterMs = 6000;
    HWND window = nullptr, status = nullptr, bar = nullptr;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD start = GetTickCount();
    std::thread thread;

    static LRESULT CALLBACK Proc(HWND w, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_TIMER) {
            KillTimer(w, wp);
            ShowWindow(w, SW_SHOW);
        }
        if (msg == WM_DESTROY) PostQuitMessage(0);
        return DefWindowProcW(w, msg, wp, lp);
    }

    Progress() {
        thread = std::thread([this] {
            // The DLL's manifest (CMakeLists.txt) asks for common controls v6, the themed progress
            // bar; a DLL's manifest only applies inside its activation context.
            HMODULE self = nullptr;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&RefreshShaders, &self);
            ACTCTXW actx = {sizeof(actx)};
            actx.dwFlags = ACTCTX_FLAG_HMODULE_VALID | ACTCTX_FLAG_RESOURCE_NAME_VALID;
            actx.hModule = self;
            actx.lpResourceName = MAKEINTRESOURCEW(2);  // ISOLATIONAWARE_MANIFEST_RESOURCE_ID
            HANDLE context = CreateActCtxW(&actx);
            ULONG_PTR cookie = 0;
            if (context != INVALID_HANDLE_VALUE) ActivateActCtx(context, &cookie);
            using InitFn = BOOL(WINAPI*)(const INITCOMMONCONTROLSEX*);
            HMODULE comctl = LoadLibraryW(L"comctl32.dll");
            INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_PROGRESS_CLASS};
            if (auto init = comctl ? (InitFn)GetProcAddress(comctl, "InitCommonControlsEx") : nullptr) init(&icc);

            WNDCLASSW wc = {};
            wc.lpfnWndProc = Proc;
            wc.hInstance = self;
            wc.hCursor = LoadCursor(nullptr, IDC_WAIT);
            wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
            wc.lpszClassName = L"LegacyGpuProgress";
            RegisterClassW(&wc);
            RECT r = {0, 0, 460, 150};
            AdjustWindowRectEx(&r, WS_POPUP | WS_CAPTION, FALSE, WS_EX_TOPMOST);
            int width = r.right - r.left, height = r.bottom - r.top;
            window = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, kTitle, WS_POPUP | WS_CAPTION,
                                     (GetSystemMetrics(SM_CXSCREEN) - width) / 2,
                                     (GetSystemMetrics(SM_CYSCREEN) - height) / 2, width, height, nullptr, nullptr,
                                     self, nullptr);
            HWND text = CreateWindowExW(0, L"STATIC",
                                        L"Converting the game's shaders for your graphics card. This happens on the "
                                        L"first start and after game updates, and takes a few minutes. The game "
                                        L"starts when it is done.",
                                        WS_CHILD | WS_VISIBLE, 16, 16, 428, 54, window, nullptr, self, nullptr);
            bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 16, 80, 428, 20, window, nullptr,
                                  self, nullptr);
            status = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE, 16, 110, 428, 24, window, nullptr,
                                     self, nullptr);
            NONCLIENTMETRICSW m = {sizeof(m)};
            SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(m), &m, 0);
            HFONT font = CreateFontIndirectW(&m.lfMessageFont);
            SendMessageW(text, WM_SETFONT, (WPARAM)font, FALSE);
            SendMessageW(status, WM_SETFONT, (WPARAM)font, FALSE);
            SetTimer(window, 1, kShowAfterMs, nullptr);
            SetEvent(ready);
            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) DispatchMessageW(&msg);
            DeleteObject(font);
            if (context != INVALID_HANDLE_VALUE) {
                DeactivateActCtx(0, cookie);
                ReleaseActCtx(context);
            }
        });
        WaitForSingleObject(ready, INFINITE);
    }

    ~Progress() {
        PostMessageW(window, WM_CLOSE, 0, 0);
        thread.join();
        CloseHandle(ready);
    }

    void Show(const wchar_t* text) { SetWindowTextW(status, text); }

    // "42% - 1400 of 3355 shaders converted, about 1 min left"
    void Set(int done, int total) {
        SendMessageW(bar, PBM_SETRANGE32, 0, total);
        SendMessageW(bar, PBM_SETPOS, done, 0);
        wchar_t s[128];
        int n = swprintf_s(s, L"%d%% - %d of %d shaders converted", total ? done * 100 / total : 0, done, total);
        DWORD elapsed = (GetTickCount() - start) / 1000;
        if (done * 20 >= total && done < total && elapsed >= 3) {  // the estimate is noise before that
            DWORD left = (DWORD)((unsigned long long)elapsed * (total - done) / done);
            if (left < 60)
                swprintf_s(s + n, ARRAYSIZE(s) - n, L", less than a minute left");
            else
                swprintf_s(s + n, ARRAYSIZE(s) - n, L", about %lu min left", (left + 30) / 60);
        }
        Show(s);
    }
};

// Runs the generator, logging its output and showing its batch progress; returns its exit code.
static DWORD RunGenerator(std::wstring cmd, Progress& progress) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE read, write;
    if (!CreatePipe(&read, &write, &sa, 0)) return GetLastError();
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    // Python's subprocess duplicates all three standard handles, so stdin must be a real one.
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    si.hStdInput = nul;
    si.hStdOutput = si.hStdError = write;
    PROCESS_INFORMATION pi = {};
    // Closing the job kills the generator and its converters if the game exits while they run.
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limit, sizeof(limit));
    BOOL started = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_SUSPENDED,
                                  nullptr, nullptr, &si, &pi);
    CloseHandle(write);
    CloseHandle(nul);
    DWORD code = started ? 0 : GetLastError();
    if (started) {
        AssignProcessToJobObject(job, pi.hProcess);
        ResumeThread(pi.hThread);
        std::string line;
        char buf[4096];
        DWORD n;
        while (ReadFile(read, buf, sizeof(buf), &n, nullptr) && n) {
            for (DWORD i = 0; i < n; ++i) {
                if (buf[i] != '\r' && buf[i] != '\n') {
                    line += buf[i];
                    continue;
                }
                int done, total;
                if (sscanf_s(line.c_str(), "%d/%d shaders", &done, &total) == 2) {
                    progress.Set(done, total);
                } else if (!line.empty()) {
                    Log("shaders: %s", line.c_str());
                }
                line.clear();
            }
        }
        WaitForSingleObject(pi.hProcess, INFINITE);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    CloseHandle(read);
    CloseHandle(job);
    return code;
}

bool RefreshShaders() {
    std::wstring dir = ProxyDir(), tools = dir + L"legacygpu_tools\\", python = tools + L"python\\python.exe";
    std::wstring shaders = dir + L"legacygpu_shaders", stamp = shaders + L"\\stamp.txt";
    if (!FileExists(python)) return true;  // a manual install: use legacygpu_shaders as it is
    const std::wstring inputs[] = {dir + L"..\\..\\Content\\Paks\\pakchunk0-WindowsNoEditor.pak",
                                   tools + L"build_shaders.py", tools + L"ms2cs.py", tools + L"fix16.py"};
    if (!FileExists(inputs[0])) {
        Log("WARN: %ls not found, shaders not refreshed", inputs[0].c_str());
        return true;
    }
    std::string print = Fingerprint(inputs, ARRAYSIZE(inputs));
    if (ReadText(stamp) == print) return true;

    wchar_t game[MAX_PATH];
    GetFullPathNameW((dir + L"..\\..\\..").c_str(), MAX_PATH, game, nullptr);
    std::wstring cmd = L"\"" + python + L"\" -I -X utf8 -u \"" + tools + L"build_shaders.py\" \"" + game + L"\" \"" +
                       shaders + L"\"";
    Log("game files changed, converting shaders: %ls", cmd.c_str());
    DWORD start = GetTickCount(), code;
    {
        Progress progress;
        progress.Show(L"Reading the game's shaders...");
        code = RunGenerator(cmd, progress);
    }
    Log("shader conversion exited with %lu after %lu s", code, (GetTickCount() - start) / 1000);
    if (code == 0) {
        FILE* f = nullptr;
        if (!_wfopen_s(&f, stamp.c_str(), L"wb")) {
            fputs(print.c_str(), f);
            fclose(f);
        }
        return true;
    }
    wchar_t text[MAX_PATH + 256];
    swprintf_s(text,
               L"Converting the game's shaders failed (code %lu). The game will start, but parts of the world may "
               L"be invisible. It tries again on the next start.\n\nDetails are in %slegacygpu.log.",
               code, dir.c_str());
    MessageBoxW(nullptr, text, kTitle, MB_OK | MB_ICONWARNING | MB_TOPMOST);
    return false;
}
