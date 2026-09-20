/*
 * Low Battery Red
 * ---------------
 * Turns the whole screen red (click-through, never takes focus) while the
 * battery is at/below a threshold AND the laptop is not charging.
 *
 * One exe, several modes:
 *   (no args)     install to %LOCALAPPDATA%\LowBatteryRed, enable autostart, start
 *   --run         supervisor: keeps the worker alive (restart on crash / hang)
 *   --worker N    worker: owns the overlay window (N = supervisor pid)
 *   --test        show the red overlay for 6 seconds, then quit
 *   --uninstall   stop everything, remove autostart + files
 *   --silent      (any mode) no message boxes
 *
 * Build: see build.sh  (MinGW-w64, no runtime DLLs needed)
 */
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */
#define EXE_NAME      L"LowBatteryRed.exe"
#define APP_TITLE     L"Low Battery Red"
#define MTX_SUP       L"Local\\LowBatteryRed.Supervisor"
#define MTX_WRK       L"Local\\LowBatteryRed.Worker"
#define EVT_STOP      L"Local\\LowBatteryRed.Stop"
#define CLS_CTL       L"LowBatteryRed.Ctl"
#define CLS_OVL       L"LowBatteryRed.Overlay"
#define TASK_NAME     L"LowBatteryRed"
#define RUN_KEY       L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"

#define DEF_THRESHOLD 20      /* percent */
#define DEF_OPACITY   55      /* percent */
#define HUNG_LIMIT    3       /* consecutive "not responding" checks before kill */

#ifdef LBR_TEST                /* test build: faster clocks + simulated battery */
  #define TICK_MS      250
  #define HEARTBEAT_MS 1000
  #define STABLE_MS    3000
#else
  #define TICK_MS      2000
  #define HEARTBEAT_MS 10000
  #define STABLE_MS    10000
#endif

static HINSTANCE g_hInst;

/* ------------------------------------------------------------------ */
/* Small helpers                                                       */
/* ------------------------------------------------------------------ */
static LONG WINAPI CrashFilter(EXCEPTION_POINTERS *ep)
{
    /* No "app has stopped working" dialog: die instantly so the
       supervisor can restart us within a second. */
    (void)ep;
    TerminateProcess(GetCurrentProcess(), 0xDEAD);
    return EXCEPTION_EXECUTE_HANDLER;
}

/* Opt out of Windows 11 "efficiency mode"/EcoQoS throttling (best effort). */
static void DisableThrottling(void)
{
    typedef struct { ULONG Version; ULONG ControlMask; ULONG StateMask; } PPTS;
    typedef BOOL (WINAPI *SetPI)(HANDLE, int, LPVOID, DWORD);
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    SetPI fn = k ? (SetPI)(void *)GetProcAddress(k, "SetProcessInformation") : NULL;
    if (fn) {
        PPTS s = { 1, 1 /*EXECUTION_SPEED*/, 0 /*opt out*/ };
        fn(GetCurrentProcess(), 4 /*ProcessPowerThrottling*/, &s, sizeof s);
    }
}

static void Say(BOOL silent, UINT icon, const WCHAR *text)
{
    if (!silent) MessageBoxW(NULL, text, APP_TITLE, MB_OK | icon | MB_TOPMOST);
}

static BOOL ObjectExists(const WCHAR *mutexName)
{
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, mutexName);
    if (h) { CloseHandle(h); return TRUE; }
    return GetLastError() == ERROR_ACCESS_DENIED;
}

/* Start a process; optionally wait for it. */
static BOOL Launch(const WCHAR *app, WCHAR *cmdline, DWORD waitMs, DWORD *exitCode)
{
    STARTUPINFOW si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si); si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(app, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return FALSE;
    if (waitMs) {
        if (WaitForSingleObject(pi.hProcess, waitMs) == WAIT_OBJECT_0) {
            if (exitCode) GetExitCodeProcess(pi.hProcess, exitCode);
        } else {
            TerminateProcess(pi.hProcess, 1);
            if (exitCode) *exitCode = 1;
        }
    } else if (exitCode) *exitCode = 0;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return TRUE;
}

static void KillOthers(void)   /* every other LowBatteryRed.exe of this user */
{
    DWORD me = GetCurrentProcessId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
    if (Process32FirstW(snap, &pe)) do {
        if (pe.th32ProcessID != me && _wcsicmp(pe.szExeFile, EXE_NAME) == 0) {
            HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
            if (h) { TerminateProcess(h, 1); CloseHandle(h); }
        }
    } while (Process32NextW(snap, &pe));
    CloseHandle(snap);
}

/* Ask running instance (supervisor + worker) to quit; force after timeout. */
static void StopRunning(DWORD timeoutMs)
{
    HANDLE e = OpenEventW(EVENT_MODIFY_STATE, FALSE, EVT_STOP);
    if (e) { SetEvent(e); CloseHandle(e); }
    DWORD t0 = GetTickCount();
    while (ObjectExists(MTX_SUP) || ObjectExists(MTX_WRK)) {
        if (GetTickCount() - t0 > timeoutMs) { KillOthers(); Sleep(800); break; }
        Sleep(100);
    }
}

/* ------------------------------------------------------------------ */
/* Config (config.ini next to the exe, re-read live)                   */
/* ------------------------------------------------------------------ */
typedef struct { int threshold; int opacity; } Config;
static WCHAR g_cfgPath[MAX_PATH];

static void InitCfgPath(void)
{
    GetModuleFileNameW(NULL, g_cfgPath, MAX_PATH);
    WCHAR *p = wcsrchr(g_cfgPath, L'\\');
    if (p) p[1] = 0;
    wcsncat(g_cfgPath, L"config.ini", MAX_PATH - wcslen(g_cfgPath) - 1);
}

static void LoadConfig(Config *c)
{
    int t = (int)GetPrivateProfileIntW(L"Settings", L"Threshold", DEF_THRESHOLD, g_cfgPath);
    int o = (int)GetPrivateProfileIntW(L"Settings", L"Opacity",   DEF_OPACITY,   g_cfgPath);
    if (t < 1)   t = 1;
    if (t > 99)  t = 99;
    if (o < 10)  o = 10;
    if (o > 100) o = 100;
    c->threshold = t; c->opacity = o;
}

/* ------------------------------------------------------------------ */
/* Battery logic                                                       */
/* ------------------------------------------------------------------ */
#ifdef LBR_TEST
/* Test hook: file "AC PCT FLAG [hang]" named by env LBR_SIM_FILE. */
static BOOL SimStatus(SYSTEM_POWER_STATUS *s)
{
    static SYSTEM_POWER_STATUS last; static BOOL have = FALSE;
    WCHAR path[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LBR_SIM_FILE", path, MAX_PATH)) return FALSE;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, 0, NULL);
    if (f != INVALID_HANDLE_VALUE) {
        char b[64] = {0}; DWORD n = 0; int ac = 1, pct = 100, fl = 0, hang = 0;
        ReadFile(f, b, 63, &n, NULL); CloseHandle(f);
        if (sscanf(b, "%d %d %d %d", &ac, &pct, &fl, &hang) >= 3) {
            if (hang) Sleep(INFINITE);
            ZeroMemory(&last, sizeof last);
            last.ACLineStatus = (BYTE)ac; last.BatteryLifePercent = (BYTE)pct; last.BatteryFlag = (BYTE)fl;
            have = TRUE;
        }
    }
    if (!have) return FALSE;
    *s = last;
    return TRUE;
}
#endif

static BOOL ReadPower(SYSTEM_POWER_STATUS *s)
{
#ifdef LBR_TEST
    if (SimStatus(s)) return TRUE;
#endif
    return GetSystemPowerStatus(s);
}

/* TRUE = the screen should be red right now. */
static BOOL WantOverlay(const Config *c, BOOL alreadyShown)
{
    SYSTEM_POWER_STATUS s;
    if (!ReadPower(&s))            return FALSE;
    if (s.BatteryFlag == 255)      return FALSE;   /* status unknown            */
    if (s.BatteryFlag & 128)       return FALSE;   /* no battery (desktop PC)   */
    if (s.ACLineStatus == 1)       return FALSE;   /* charger connected         */
    if (s.BatteryFlag & 8)         return FALSE;   /* charging                  */
    if (s.BatteryFlag & 4)         return TRUE;    /* critical (<5%), unplugged */
    if (s.BatteryLifePercent > 100) return FALSE;  /* percentage unknown        */
    /* +2% hysteresis so a jittery reading can't make the screen flicker */
    return (int)s.BatteryLifePercent <= c->threshold + (alreadyShown ? 2 : 0);
}

/* ------------------------------------------------------------------ */
/* Overlay window                                                      */
/* ------------------------------------------------------------------ */
static HWND   g_ctl, g_ovl;
static HBRUSH g_brush;
static BOOL   g_shown, g_force;
static int    g_alphaApplied = -1;

static LRESULT CALLBACK OvlProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_NCHITTEST:    return HTTRANSPARENT;   /* mouse goes to the window below */
    case WM_MOUSEACTIVATE:return MA_NOACTIVATE;   /* never take focus               */
    case WM_ERASEBKGND: {
        RECT rc; GetClientRect(h, &rc);
        FillRect((HDC)w, &rc, g_brush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        FillRect(dc, &ps.rcPaint, g_brush);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

static BOOL EnsureOverlay(void)
{
    if (g_ovl && IsWindow(g_ovl)) return TRUE;
    g_ovl = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
        CLS_OVL, L"", WS_POPUP, 0, 0, 1, 1, NULL, NULL, g_hInst, NULL);
    g_alphaApplied = -1;
    return g_ovl != NULL;
}

static void ShowOverlay(const Config *c)
{
    if (!EnsureOverlay()) return;
    int a = c->opacity * 255 / 100;
    if (a != g_alphaApplied) {
        SetLayeredWindowAttributes(g_ovl, 0, (BYTE)a, LWA_ALPHA);
        g_alphaApplied = a;
    }
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN),  y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) { x = 0; y = 0; w = GetSystemMetrics(SM_CXSCREEN); h = GetSystemMetrics(SM_CYSCREEN); }
    /* NOACTIVATE: showing / re-raising can never steal keyboard focus */
    SetWindowPos(g_ovl, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    g_shown = TRUE;
}

static void HideOverlay(void)
{
    if (g_ovl && IsWindow(g_ovl)) ShowWindow(g_ovl, SW_HIDE);
    g_shown = FALSE;
}

static void Update(void)
{
    Config c; LoadConfig(&c);
    if (g_force || WantOverlay(&c, g_shown)) ShowOverlay(&c);   /* also re-asserts topmost + size */
    else if (g_shown)                        HideOverlay();
}

static LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    switch (m) {
    case WM_CREATE:  SetTimer(h, 1, TICK_MS, NULL); return 0;
    case WM_TIMER:
        if (w == 1) Update();
        else if (w == 2) PostQuitMessage(0);          /* --test finished */
        return 0;
    case WM_POWERBROADCAST:
        if (w == PBT_APMPOWERSTATUSCHANGE || w == PBT_APMRESUMEAUTOMATIC || w == PBT_APMRESUMESUSPEND)
            Update();
        return TRUE;
    case WM_DISPLAYCHANGE:
        if (g_shown) Update();                        /* resize to new screen layout */
        return 0;
    case WM_QUERYENDSESSION: return TRUE;             /* never block logoff/shutdown */
    }
    return DefWindowProcW(h, m, w, l);
}

/* ------------------------------------------------------------------ */
/* Worker                                                              */
/* ------------------------------------------------------------------ */
static void SpawnSelf(const WCHAR *args)
{
    WCHAR self[MAX_PATH], cmd[MAX_PATH + 64];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    _snwprintf(cmd, MAX_PATH + 63, L"\"%ls\" %ls", self, args);
    cmd[MAX_PATH + 63] = 0;
    Launch(self, cmd, 0, NULL);
}

static int RunWorker(DWORD supervisorPid, BOOL testMode)
{
    HANDLE mtx = NULL, stop = NULL, parent = NULL;
    int rc = 0;

    if (!testMode) {
        mtx = CreateMutexW(NULL, FALSE, MTX_WRK);
        if (!mtx || GetLastError() == ERROR_ALREADY_EXISTS) return 3;   /* another worker is alive */
        stop = CreateEventW(NULL, TRUE, FALSE, EVT_STOP);
        if (supervisorPid) parent = OpenProcess(SYNCHRONIZE, FALSE, supervisorPid);
    }

    InitCfgPath();
    g_brush = CreateSolidBrush(RGB(255, 0, 0));

    WNDCLASSEXW wc; ZeroMemory(&wc, sizeof wc); wc.cbSize = sizeof wc;
    wc.hInstance = g_hInst; wc.hCursor = NULL;
    wc.lpfnWndProc = OvlProc; wc.lpszClassName = CLS_OVL; wc.hbrBackground = g_brush;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = CtlProc; wc.lpszClassName = CLS_CTL; wc.hbrBackground = NULL;
    RegisterClassExW(&wc);

    /* Hidden top-level window (NOT message-only, so it receives power/display broadcasts). */
    g_ctl = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, CLS_CTL, L"", WS_POPUP,
                            0, 0, 0, 0, NULL, NULL, g_hInst, NULL);
    if (!g_ctl) return 4;

    if (testMode) { g_force = TRUE; SetTimer(g_ctl, 2, 6000, NULL); }
    Update();

    HANDLE hs[2]; DWORD n = 0;
    if (stop)   hs[n++] = stop;
    if (parent) hs[n++] = parent;

    for (;;) {
        DWORD r = MsgWaitForMultipleObjects(n, hs, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_FAILED) break;
        if (r < WAIT_OBJECT_0 + n) {
            HANDLE which = hs[r - WAIT_OBJECT_0];
            if (which == stop) break;                                   /* clean stop */
            /* supervisor died without a stop request -> bring it back */
            if (stop && WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) SpawnSelf(L"--run");
            break;
        }
        MSG msg; BOOL quit = FALSE;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = TRUE; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        if (quit) break;
    }

    if (g_ovl) DestroyWindow(g_ovl);
    if (g_ctl) DestroyWindow(g_ctl);
    if (parent) CloseHandle(parent);
    if (stop) CloseHandle(stop);
    if (mtx) CloseHandle(mtx);
    return rc;
}

/* ------------------------------------------------------------------ */
/* Supervisor                                                          */
/* ------------------------------------------------------------------ */
static int RunSupervisor(void)
{
    HANDLE mtx = CreateMutexW(NULL, FALSE, MTX_SUP);
    if (!mtx || GetLastError() == ERROR_ALREADY_EXISTS) return 0;   /* already running: fine */
    HANDLE stop = CreateEventW(NULL, TRUE, FALSE, EVT_STOP);
    if (!stop) return 1;

    WCHAR self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    DWORD delay = 1000;

    for (;;) {
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) break;

        WCHAR cmd[MAX_PATH + 64];
        _snwprintf(cmd, MAX_PATH + 63, L"\"%ls\" --worker %lu", self, GetCurrentProcessId());
        cmd[MAX_PATH + 63] = 0;

        STARTUPINFOW si; PROCESS_INFORMATION pi;
        ZeroMemory(&si, sizeof si); si.cb = sizeof si; ZeroMemory(&pi, sizeof pi);
        DWORD started = GetTickCount();
        BOOL stopping = FALSE;

        if (CreateProcessW(self, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
            HANDLE h[2] = { stop, pi.hProcess };
            int hung = 0;
            for (;;) {
                DWORD w = WaitForMultipleObjects(2, h, FALSE, HEARTBEAT_MS);
                if (w == WAIT_OBJECT_0) {                       /* stop requested */
                    stopping = TRUE;
                    if (WaitForSingleObject(pi.hProcess, 4000) != WAIT_OBJECT_0)
                        TerminateProcess(pi.hProcess, 1);
                    break;
                }
                if (w == WAIT_OBJECT_0 + 1) break;              /* worker exited / crashed */
                if (w == WAIT_TIMEOUT) {                        /* heartbeat: is it frozen? */
                    HWND cw = FindWindowW(CLS_CTL, NULL);
                    DWORD pid = 0;
                    if (cw) GetWindowThreadProcessId(cw, &pid);
                    if (cw && pid == pi.dwProcessId && IsHungAppWindow(cw)) hung++; else hung = 0;
                    if (hung >= HUNG_LIMIT) {
                        TerminateProcess(pi.hProcess, 9);
                        WaitForSingleObject(pi.hProcess, 5000);
                        break;
                    }
                } else break;                                   /* WAIT_FAILED */
            }
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        if (stopping) break;

        /* restart back-off: quick crash loops slow down, healthy runs reset it */
        if (GetTickCount() - started >= STABLE_MS) delay = 1000;
        else { delay *= 2; if (delay > 30000) delay = 30000; }
        if (WaitForSingleObject(stop, delay) == WAIT_OBJECT_0) break;
    }
    CloseHandle(stop); CloseHandle(mtx);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Install / uninstall                                                 */
/* ------------------------------------------------------------------ */
static BOOL GetInstallDir(WCHAR *out /* MAX_PATH */)
{
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", out, MAX_PATH - 40);
    if (!n || n >= MAX_PATH - 40) return FALSE;
    wcscat(out, L"\\LowBatteryRed");
    return TRUE;
}

static void XmlEsc(const WCHAR *in, WCHAR *out, size_t cap)
{
    size_t o = 0;
    for (; *in && o + 8 < cap; in++) {
        const WCHAR *rep = NULL;
        switch (*in) {
            case L'&': rep = L"&amp;"; break;  case L'<': rep = L"&lt;"; break;
            case L'>': rep = L"&gt;";  break;  case L'"': rep = L"&quot;"; break;
        }
        if (rep) { wcscpy(out + o, rep); o += wcslen(rep); } else out[o++] = *in;
    }
    out[o] = 0;
}

static BOOL SchTasks(const WCHAR *args)
{
    WCHAR sys[MAX_PATH], exe[MAX_PATH], cmd[2048];
    if (!GetSystemDirectoryW(sys, MAX_PATH)) return FALSE;
    _snwprintf(exe, MAX_PATH - 1, L"%ls\\schtasks.exe", sys); exe[MAX_PATH - 1] = 0;
    _snwprintf(cmd, 2047, L"\"%ls\" %ls", exe, args); cmd[2047] = 0;
    DWORD code = 1;
    if (!Launch(exe, cmd, 30000, &code)) return FALSE;
    return code == 0;
}

/* Scheduled task = safety net: (re)starts the supervisor at logon and every
   minute (a 2nd copy exits instantly). Battery restrictions are switched OFF,
   otherwise Windows would kill the task exactly when the laptop is unplugged. */
static BOOL CreateTask(const WCHAR *exePath, BOOL withLogon)
{
    WCHAR user[256] = L"", dom[128], nm[128], uEsc[600], pEsc[2 * MAX_PATH], dEsc[2 * MAX_PATH];
    if (GetEnvironmentVariableW(L"USERNAME", nm, 128) && GetEnvironmentVariableW(L"USERDOMAIN", dom, 128))
        _snwprintf(user, 255, L"%ls\\%ls", dom, nm);
    if (withLogon && !user[0]) return FALSE;
    XmlEsc(user, uEsc, 600);
    XmlEsc(exePath, pEsc, 2 * MAX_PATH);

    WCHAR dirOnly[MAX_PATH]; wcscpy(dirOnly, exePath);
    WCHAR *sl = wcsrchr(dirOnly, L'\\'); if (sl) *sl = 0;
    XmlEsc(dirOnly, dEsc, 2 * MAX_PATH);

    WCHAR *xml = (WCHAR *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 16384 * sizeof(WCHAR));
    if (!xml) return FALSE;

    WCHAR logon[900] = L"", principalUser[700] = L"";
    if (withLogon) {
        _snwprintf(logon, 899,
            L"<LogonTrigger><Repetition><Interval>PT1M</Interval><StopAtDurationEnd>false</StopAtDurationEnd></Repetition>"
            L"<Enabled>true</Enabled><UserId>%ls</UserId></LogonTrigger>", uEsc);
        _snwprintf(principalUser, 699, L"<UserId>%ls</UserId>", uEsc);
    }
    _snwprintf(xml, 16383,
        L"<?xml version=\"1.0\" encoding=\"UTF-16\"?>\r\n"
        L"<Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\">\r\n"
        L"<RegistrationInfo><Description>Keeps Low Battery Red running.</Description></RegistrationInfo>\r\n"
        L"<Triggers>%ls"
        L"<TimeTrigger><Repetition><Interval>PT1M</Interval><StopAtDurationEnd>false</StopAtDurationEnd></Repetition>"
        L"<StartBoundary>2024-01-01T00:00:00</StartBoundary><Enabled>true</Enabled></TimeTrigger>"
        L"</Triggers>\r\n"
        L"<Principals><Principal id=\"Author\">%ls<LogonType>InteractiveToken</LogonType>"
        L"<RunLevel>LeastPrivilege</RunLevel></Principal></Principals>\r\n"
        L"<Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>"
        L"<DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>"
        L"<StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>"
        L"<AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>true</StartWhenAvailable>"
        L"<RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable>"
        L"<IdleSettings><StopOnIdleEnd>false</StopOnIdleEnd><RestartOnIdle>false</RestartOnIdle></IdleSettings>"
        L"<AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden>"
        L"<RunOnlyIfIdle>false</RunOnlyIfIdle><WakeToRun>false</WakeToRun>"
        L"<ExecutionTimeLimit>PT0S</ExecutionTimeLimit><Priority>5</Priority>"
        L"<RestartOnFailure><Interval>PT1M</Interval><Count>999</Count></RestartOnFailure></Settings>\r\n"
        L"<Actions Context=\"Author\"><Exec><Command>%ls</Command><Arguments>--run</Arguments>"
        L"<WorkingDirectory>%ls</WorkingDirectory></Exec></Actions>\r\n"
        L"</Task>\r\n",
        logon, principalUser, pEsc, dEsc);

    WCHAR tmpDir[MAX_PATH], tmp[MAX_PATH];
    BOOL ok = FALSE;
    if (GetTempPathW(MAX_PATH, tmpDir)) {          /* GetTempPath already ends with a backslash */
        _snwprintf(tmp, MAX_PATH - 1, L"%lslowbatteryred_task.xml", tmpDir); tmp[MAX_PATH - 1] = 0;
        HANDLE f = CreateFileW(tmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD w; WORD bom = 0xFEFF;
            WriteFile(f, &bom, 2, &w, NULL);
            WriteFile(f, xml, (DWORD)(wcslen(xml) * sizeof(WCHAR)), &w, NULL);
            CloseHandle(f);
            WCHAR args[MAX_PATH + 100];
            _snwprintf(args, MAX_PATH + 99, L"/Create /TN \"%ls\" /XML \"%ls\" /F", TASK_NAME, tmp);
            args[MAX_PATH + 99] = 0;
            ok = SchTasks(args);
            DeleteFileW(tmp);
        }
    }
    HeapFree(GetProcessHeap(), 0, xml);
    return ok;
}

static void DeleteTask(void)
{
    WCHAR args[200];
    _snwprintf(args, 199, L"/Delete /TN \"%ls\" /F", TASK_NAME); args[199] = 0;
    SchTasks(args);
}

static BOOL SetRunKey(const WCHAR *exePath)
{
    HKEY k; WCHAR val[MAX_PATH + 32];
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, NULL, 0, KEY_SET_VALUE, NULL, &k, NULL) != ERROR_SUCCESS)
        return FALSE;
    _snwprintf(val, MAX_PATH + 31, L"\"%ls\" --run", exePath); val[MAX_PATH + 31] = 0;
    LONG r = RegSetValueExW(k, TASK_NAME, 0, REG_SZ, (const BYTE *)val, (DWORD)((wcslen(val) + 1) * sizeof(WCHAR)));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static void DelRunKey(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
        RegDeleteValueW(k, TASK_NAME);
        RegCloseKey(k);
    }
}

static int DoInstall(BOOL silent)
{
    WCHAR self[MAX_PATH], dir[MAX_PATH], dst[MAX_PATH], cfg[MAX_PATH], ads[MAX_PATH + 32];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    if (!GetInstallDir(dir)) { Say(silent, MB_ICONERROR, L"Cannot locate %LOCALAPPDATA%."); return 1; }
    CreateDirectoryW(dir, NULL);
    _snwprintf(dst, MAX_PATH - 1, L"%ls\\%ls", dir, EXE_NAME); dst[MAX_PATH - 1] = 0;
    _snwprintf(cfg, MAX_PATH - 1, L"%ls\\config.ini", dir);    cfg[MAX_PATH - 1] = 0;
    BOOL same = _wcsicmp(self, dst) == 0;

    DeleteTask();                 /* so the scheduler can't relaunch during the upgrade */
    StopRunning(6000);

    if (!same) {
        BOOL copied = FALSE;
        for (int i = 0; i < 20 && !copied; i++) {
            copied = CopyFileW(self, dst, FALSE);
            if (!copied) Sleep(300);
        }
        if (!copied) {
            Say(silent, MB_ICONERROR, L"Could not copy the program into %LOCALAPPDATA%\\LowBatteryRed.\n"
                                      L"Close it in Task Manager and try again.");
            return 1;
        }
        /* drop the "downloaded from internet" mark so autostart never shows a security prompt */
        _snwprintf(ads, MAX_PATH + 31, L"%ls:Zone.Identifier", dst); ads[MAX_PATH + 31] = 0;
        DeleteFileW(ads);
    }

    if (GetFileAttributesW(cfg) == INVALID_FILE_ATTRIBUTES) {
        WritePrivateProfileStringW(L"Settings", L"Threshold", L"20", cfg);
        WritePrivateProfileStringW(L"Settings", L"Opacity",   L"55", cfg);
    }

    BOOL runKey = SetRunKey(dst);
    BOOL task = CreateTask(dst, TRUE) || CreateTask(dst, FALSE);

    WCHAR cmd[MAX_PATH + 32];
    _snwprintf(cmd, MAX_PATH + 31, L"\"%ls\" --run", dst); cmd[MAX_PATH + 31] = 0;
    BOOL started = Launch(dst, cmd, 0, NULL);

    if (!started || !(runKey || task)) {
        Say(silent, MB_ICONWARNING,
            started ? L"The program is running, but automatic start at sign-in could not be set up."
                    : L"Installed, but the program could not be started.");
        return 1;
    }
    Say(silent, MB_ICONINFORMATION,
        L"Low Battery Red is installed and running.\n\n"
        L"\u2022 Screen turns red when the battery is at or below 20% and NOT charging.\n"
        L"\u2022 Starts automatically at every sign-in and restarts itself if it ever crashes.\n"
        L"\u2022 It never takes focus and clicks pass straight through it.\n\n"
        L"Settings (threshold / strength): %LOCALAPPDATA%\\LowBatteryRed\\config.ini\n"
        L"To remove: run Uninstall.bat");
    return 0;
}

/* A just-exited process (or an antivirus scan) can keep a file locked for a few
   milliseconds. Retry briefly instead of leaving files behind. */
static void DeleteRetry(const WCHAR *path, DWORD ms)
{
    for (DWORD t0 = GetTickCount();;) {
        if (DeleteFileW(path)) return;
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return;
        if (GetTickCount() - t0 >= ms) return;
        Sleep(150);
    }
}

static void RemoveDirRetry(const WCHAR *path, DWORD ms)
{
    for (DWORD t0 = GetTickCount();;) {
        if (RemoveDirectoryW(path)) return;
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return;
        if (GetTickCount() - t0 >= ms) return;
        Sleep(150);
    }
}

static int DoUninstall(BOOL silent)
{
    WCHAR self[MAX_PATH], dir[MAX_PATH], f[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);
    DeleteTask();
    DelRunKey();
    StopRunning(6000);
    if (GetInstallDir(dir)) {
        _snwprintf(f, MAX_PATH - 1, L"%ls\\%ls", dir, EXE_NAME); f[MAX_PATH - 1] = 0;
        if (_wcsicmp(self, f) != 0) DeleteRetry(f, 5000);
        else {                                    /* running from the install dir: delete after we exit */
            WCHAR cmd[MAX_PATH * 2 + 80], sys[MAX_PATH];
            GetSystemDirectoryW(sys, MAX_PATH);
            _snwprintf(cmd, MAX_PATH * 2 + 79,
                       L"\"%ls\\cmd.exe\" /c ping -n 3 127.0.0.1 >nul & del /f /q \"%ls\" & rmdir \"%ls\"",
                       sys, f, dir);
            cmd[MAX_PATH * 2 + 79] = 0;
            Launch(NULL, cmd, 0, NULL);
        }
        _snwprintf(f, MAX_PATH - 1, L"%ls\\config.ini", dir); f[MAX_PATH - 1] = 0;
        DeleteRetry(f, 2000);
        RemoveDirRetry(dir, 3000);
    }
    Say(silent, MB_ICONINFORMATION, L"Low Battery Red has been stopped and removed.");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */
int WINAPI wWinMain(HINSTANCE hi, HINSTANCE hp, LPWSTR cl, int sw)
{
    (void)hp; (void)cl; (void)sw;
    g_hInst = hi;
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    SetUnhandledExceptionFilter(CrashFilter);
    DisableThrottling();

    int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    BOOL silent = FALSE; const WCHAR *mode = NULL; DWORD num = 0;
    for (int i = 1; argv && i < argc; i++) {
        if (_wcsicmp(argv[i], L"--silent") == 0) silent = TRUE;
        else if (!mode) mode = argv[i];
        else num = (DWORD)_wtoi(argv[i]);
    }

    if (!mode)                                   return DoInstall(silent);
    if (_wcsicmp(mode, L"--run") == 0)           return RunSupervisor();
    if (_wcsicmp(mode, L"--worker") == 0)        return RunWorker(num, FALSE);
    if (_wcsicmp(mode, L"--test") == 0)          return RunWorker(0, TRUE);
    if (_wcsicmp(mode, L"--uninstall") == 0)     return DoUninstall(silent);

    Say(silent, MB_ICONINFORMATION,
        L"Usage:\n  LowBatteryRed.exe              install + start\n"
        L"  LowBatteryRed.exe --test       show red screen for 6 seconds\n"
        L"  LowBatteryRed.exe --uninstall  remove");
    return 2;
}
