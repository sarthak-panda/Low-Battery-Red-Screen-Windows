#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <wchar.h>

#define CLS_CTL L"LowBatteryRed.Ctl"
#define CLS_OVL L"LowBatteryRed.Overlay"
#define EXE     L"LowBatteryRed_test.exe"
#define IEXE    L"LowBatteryRed.exe"

static int g_pass, g_fail;
static WCHAR g_sim[MAX_PATH], g_dir[MAX_PATH];

static void Check(int ok, const char *msg)
{
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", msg); fflush(stdout);
    if (ok) g_pass++; else g_fail++;
}

static void Sim(int ac, int pct, int flag, int hang)
{
    WCHAR tmp[MAX_PATH]; _snwprintf(tmp, MAX_PATH - 1, L"%ls.tmp", g_sim);
    FILE *f = _wfopen(tmp, L"w"); fprintf(f, "%d %d %d %d", ac, pct, flag, hang); fclose(f);
    MoveFileExW(tmp, g_sim, MOVEFILE_REPLACE_EXISTING);
}

static HWND Ovl(void) { return FindWindowW(CLS_OVL, NULL); }
static BOOL OvlVisible(void) { HWND o = Ovl(); return o && IsWindowVisible(o); }
static DWORD WorkerPid(void) { HWND h = FindWindowW(CLS_CTL, NULL); DWORD p = 0; if (h) GetWindowThreadProcessId(h, &p); return p; }

/* pids of all exes named `name`; returns count */
static int Procs(const WCHAR *name, DWORD *pids, int max)
{
    int n = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W pe; pe.dwSize = sizeof pe;
    if (Process32FirstW(s, &pe)) do {
        if (_wcsicmp(pe.szExeFile, name) == 0 && n < max) pids[n++] = pe.th32ProcessID;
    } while (Process32NextW(s, &pe));
    CloseHandle(s);
    return n;
}
static int CountAll(void) { DWORD p[16]; return Procs(EXE, p, 16) + Procs(IEXE, p, 16); }

static DWORD SupervisorPid(const WCHAR *name)
{
    DWORD p[16]; int n = Procs(name, p, 16), w = WorkerPid();
    for (int i = 0; i < n; i++) if ((int)p[i] != w) return p[i];
    return 0;
}

static BOOL WaitOvl(BOOL wantVisible, int ms)
{
    for (int t = 0; t < ms; t += 50) { if (OvlVisible() == wantVisible) return TRUE; Sleep(50); }
    return FALSE;
}

static BOOL WaitCount(int n, int ms)              /* number of LowBatteryRed*.exe processes */
{
    for (int t = 0; t < ms; t += 50) { if (CountAll() == n) return TRUE; Sleep(50); }
    return FALSE;
}
static BOOL WaitNoOvl(int ms)
{
    for (int t = 0; t < ms; t += 50) { if (!Ovl()) return TRUE; Sleep(50); }
    return FALSE;
}
static BOOL WaitAlpha(int want, int ms)
{
    for (int t = 0; t < ms; t += 50) {
        HWND o = Ovl(); BYTE a = 0; DWORD f = 0; COLORREF k;
        if (o && GetLayeredWindowAttributes(o, &k, &a, &f) && a == want) return TRUE;
        Sleep(50);
    }
    return FALSE;
}
static BOOL WaitGone(const WCHAR *path, int ms)   /* file/dir eventually disappears */
{
    for (int t = 0; t < ms; t += 50) { if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return TRUE; Sleep(50); }
    return FALSE;
}

/* Set a battery state, wait (generously - CI machines are slow) until the screen
   reaches the expected state, then require that it STAYS there for 800 ms
   (catches flicker / late flips). */
static void Expect(const char *name, int ac, int pct, int flag, BOOL shown)
{
    Sim(ac, pct, flag, 0);
    BOOL ok = WaitOvl(shown, 5000);
    for (DWORD t0 = GetTickCount(); ok && GetTickCount() - t0 < 800; Sleep(20))
        if (OvlVisible() != shown) ok = FALSE;
    char m[160]; snprintf(m, sizeof m, "%-52s -> screen %s", name, shown ? "RED" : "normal");
    Check(ok, m);
}

static void KillPid(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (h) { TerminateProcess(h, 1); WaitForSingleObject(h, 3000); CloseHandle(h); }
}

static BOOL RunExe(const WCHAR *args, DWORD waitMs, const WCHAR *exe)
{
    WCHAR cmd[MAX_PATH * 2];
    _snwprintf(cmd, MAX_PATH * 2 - 1, L"\"%ls\" %ls", exe, args);
    STARTUPINFOW si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) return FALSE;
    if (waitMs) WaitForSingleObject(pi.hProcess, waitMs);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return TRUE;
}

static LRESULT CALLBACK FocusProc(HWND h, UINT m, WPARAM w, LPARAM l)
{ if (m == WM_DESTROY) PostQuitMessage(0); return DefWindowProcW(h, m, w, l); }

static void Pump(int ms) { DWORD t0 = GetTickCount(); MSG m; while (GetTickCount() - t0 < (DWORD)ms) { while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&m); DispatchMessageW(&m); } Sleep(10); } }

int wmain(void)
{
    GetCurrentDirectoryW(MAX_PATH, g_dir);
    _snwprintf(g_sim, MAX_PATH - 1, L"%ls\\sim.txt", g_dir);
    SetEnvironmentVariableW(L"LBR_SIM_FILE", g_sim);
    Sim(1, 80, 0, 0);                                   /* plugged in, 80% */

    printf("\n=== 1. Startup, single instance ===\n");
    RunExe(L"--run", 0, EXE);
    DWORD t0 = GetTickCount();
    for (int i = 0; i < 400 && !WorkerPid(); i++) Sleep(50);
    printf("  (worker window appeared after %lu ms)\n", GetTickCount() - t0);
    Check(WorkerPid() != 0, "worker started by supervisor");
    Check(WaitCount(2, 5000), "exactly 2 processes (supervisor + worker)");
    RunExe(L"--run", 15000, EXE); RunExe(L"--run", 15000, EXE);     /* extra copies must exit by themselves */
    Sleep(300);
    Check(WaitCount(2, 5000), "launching --run twice more does not create duplicates");
    Check(!OvlVisible(), "plugged in @80% -> no red screen");

    printf("\n=== 2. Overlay properties (unplugged, 15%%) ===\n");
    WNDCLASSW wc = {0}; wc.lpfnWndProc = FocusProc; wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"FocusTest"; RegisterClassW(&wc);
    HWND app = CreateWindowExW(0, L"FocusTest", L"my work window", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                               50, 50, 500, 400, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(app); SetFocus(app); Pump(300);
    HWND fg0 = GetForegroundWindow(); HWND foc0 = GetFocus();
    Check(fg0 == app && foc0 == app, "harness 'work window' has foreground+focus before overlay");

    Sim(0, 15, 0, 0);
    Check(WaitOvl(TRUE, 10000), "unplugged @15% -> overlay appears");
    Pump(300);
    HWND o = Ovl();
    LONG ex = (LONG)GetWindowLongW(o, GWL_EXSTYLE);
    RECT r; GetWindowRect(o, &r);
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    printf("  (virtual screen %dx%d at %d,%d; overlay %ldx%ld at %ld,%ld)\n", vw, vh, vx, vy,
           r.right - r.left, r.bottom - r.top, r.left, r.top);
    Check(r.left == vx && r.top == vy && r.right - r.left == vw && r.bottom - r.top == vh, "overlay covers the entire screen exactly");
    Check((ex & WS_EX_TOPMOST) != 0,     "style: topmost");
    Check((ex & WS_EX_NOACTIVATE) != 0,  "style: no-activate");
    Check((ex & WS_EX_TRANSPARENT) != 0, "style: click-through");
    Check((ex & WS_EX_LAYERED) != 0,     "style: layered (translucent)");
    Check((ex & WS_EX_TOOLWINDOW) != 0,  "style: tool window (no taskbar / Alt-Tab entry)");
    BYTE a = 0; DWORD fl = 0; COLORREF key;
    GetLayeredWindowAttributes(o, &key, &a, &fl);
    Check((fl & LWA_ALPHA) && a == 140, "opacity = 55% (alpha 140)");
    Check(SendMessageW(o, WM_NCHITTEST, 0, MAKELPARAM(300, 300)) == (LRESULT)HTTRANSPARENT, "hit-test returns HTTRANSPARENT (mouse passes through)");
    Check(SendMessageW(o, WM_MOUSEACTIVATE, 0, 0) == MA_NOACTIVATE, "mouse-activate returns MA_NOACTIVATE");
    Check(GetForegroundWindow() == fg0, "FOCUS: foreground window unchanged after overlay appeared");
    Check(GetFocus() == foc0, "FOCUS: keyboard focus unchanged after overlay appeared");
    Check(GetActiveWindow() == app, "FOCUS: active window unchanged");
    POINT pt = { 300, 300 };
    Check(WindowFromPoint(pt) != o, "WindowFromPoint(300,300) is NOT the overlay (click lands on the app below)");

    /* positive control: proves this harness CAN detect a focus steal */
    HWND thief = CreateWindowExW(0, L"FocusTest", L"thief", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                 700, 100, 200, 200, NULL, NULL, wc.hInstance, NULL);
    SetForegroundWindow(thief); Pump(300);
    Check(GetForegroundWindow() != fg0, "control: a normal window DOES change foreground (harness is sensitive)");
    DestroyWindow(thief); SetForegroundWindow(app); Pump(300);

    printf("\n=== 3. Battery logic ===\n");
    Expect("unplugged 15%", 0, 15, 0, TRUE);
    Expect("plug in charger @15% (AC=1)", 1, 15, 0, FALSE);
    Expect("charging flag @15% (AC=0, flag 8)", 0, 15, 8, FALSE);
    Expect("unplugged 21% (just above threshold 20)", 0, 21, 0, FALSE);
    Expect("unplugged 20% (== threshold)", 0, 20, 0, TRUE);
    Expect("21% while red (hysteresis, no flicker)", 0, 21, 0, TRUE);
    Expect("22% while red (hysteresis edge)", 0, 22, 0, TRUE);
    Expect("23% while red (recovered -> off)", 0, 23, 0, FALSE);
    Expect("desktop PC: no battery (flag 128)", 0, 255, 128, FALSE);
    Expect("status unknown (flag 255)", 0, 255, 255, FALSE);
    Expect("percent unknown, not critical", 0, 255, 1, FALSE);
    Expect("critical flag (<5%), unplugged", 0, 3, 5, TRUE);
    Expect("critical but charger connected", 1, 3, 5, FALSE);
    Expect("100% unplugged", 0, 100, 1, FALSE);
    Check(GetForegroundWindow() == app, "FOCUS: still unchanged after many show/hide cycles");

    printf("\n=== 4. Crash recovery ===\n");
    Sim(0, 10, 0, 0); WaitOvl(TRUE, 8000);
    DWORD w1 = WorkerPid(), s1 = SupervisorPid(EXE);
    Check(w1 && s1, "have worker + supervisor pids");
    KillPid(w1);
    BOOL back = FALSE; DWORD w2 = 0;
    for (int i = 0; i < 200; i++) { w2 = WorkerPid(); if (w2 && w2 != w1 && OvlVisible()) { back = TRUE; break; } Sleep(50); }
    Check(back, "worker auto-restarted and red screen is back (<10s)");
    Check(SupervisorPid(EXE) == s1, "same supervisor kept running");
    Check(GetForegroundWindow() == app, "FOCUS: unchanged through crash+restart");

    printf("\n=== 5. Supervisor killed -> revived by worker ===\n");
    DWORD s2 = SupervisorPid(EXE), w3 = WorkerPid();
    KillPid(s2);
    BOOL rev = FALSE;
    for (int i = 0; i < 300; i++) {
        DWORD ns = SupervisorPid(EXE), nw = WorkerPid();
        if (ns && ns != s2 && nw && CountAll() == 2 && OvlVisible()) { rev = TRUE; break; }
        Sleep(50);
    }
    Check(rev, "new supervisor + single worker back, red screen still up");
    Check(WorkerPid() != 0 && CountAll() == 2, "no duplicate processes after revival");
    (void)w3;

    printf("\n=== 6. Hang detection ===\n");
    DWORD w4 = WorkerPid();
    Sim(0, 10, 0, 1);                                    /* worker freezes on next tick */
    HANDLE hp = OpenProcess(SYNCHRONIZE, FALSE, w4);
    DWORD wr = hp ? WaitForSingleObject(hp, 25000) : WAIT_FAILED;
    Sim(0, 10, 0, 0);                                    /* un-freeze before the replacement starts */
    if (hp) CloseHandle(hp);
    Check(wr == WAIT_OBJECT_0, "frozen worker was detected and killed by supervisor");
    for (int i = 0; i < 300; i++) { if (WorkerPid() && WorkerPid() != w4 && OvlVisible()) break; Sleep(50); }
    Check(WorkerPid() != 0 && WorkerPid() != w4 && OvlVisible(), "replacement worker running, red screen restored");

    printf("\n=== 7. Plug in after recovery ===\n");
    Sim(1, 80, 0, 0);
    Check(WaitOvl(FALSE, 5000), "charger connected -> overlay gone");

    printf("\n=== 8. Clean stop + --test mode ===\n");
    RunExe(L"--uninstall --silent", 30000, EXE);
    Check(WaitCount(0, 8000), "--uninstall stops supervisor + worker (0 processes left)");
    Check(WaitNoOvl(3000), "no overlay window left behind");

    RunExe(L"--test", 0, EXE);
    Check(WaitOvl(TRUE, 10000), "--test: overlay shown regardless of battery");
    Check(WaitOvl(FALSE, 9000) || Ovl() == NULL, "--test: overlay disappears by itself after ~6s");
    Check(WaitCount(0, 5000), "--test process exited");


    printf("\n=== 9. Install / live config / upgrade / uninstall ===\n");
    WCHAR la[MAX_PATH], inst[MAX_PATH], cfg[MAX_PATH], self[MAX_PATH];
    GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH);
    _snwprintf(inst, MAX_PATH - 1, L"%ls\\LowBatteryRed\\LowBatteryRed.exe", la);
    _snwprintf(cfg,  MAX_PATH - 1, L"%ls\\LowBatteryRed\\config.ini", la);
    GetModuleFileNameW(NULL, self, MAX_PATH);
    Sim(0, 40, 0, 0);                                    /* unplugged 40% = no alarm at default 20% */
    Check(RunExe(L"--silent", 60000, EXE), "installer ran (no args = install)");
    Check(WaitCount(2, 10000), "installed copy started");
    Check(GetFileAttributesW(inst) != INVALID_FILE_ATTRIBUTES, "exe copied to %LOCALAPPDATA%\\LowBatteryRed");
    Check(GetFileAttributesW(cfg) != INVALID_FILE_ATTRIBUTES, "default config.ini created");
    WCHAR thr[16]; GetPrivateProfileStringW(L"Settings", L"Threshold", L"?", thr, 16, cfg);
    Check(wcscmp(thr, L"20") == 0, "config.ini: Threshold=20");
    WCHAR rv[MAX_PATH * 2] = L""; DWORD rl = sizeof rv;
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"LowBatteryRed", RRF_RT_REG_SZ, NULL, rv, &rl);
    WCHAR want[MAX_PATH * 2]; _snwprintf(want, MAX_PATH * 2 - 1, L"\"%ls\" --run", inst);
    Check(_wcsicmp(rv, want) == 0, "autostart: HKCU Run key points at installed exe with --run");
    { DWORD p[16]; Check(Procs(IEXE, p, 16) == 2 && Procs(EXE, p, 16) == 0, "installed copy running: 1 supervisor + 1 worker"); }
    Check(!OvlVisible(), "unplugged @40% -> no overlay (above 20%)");

    WritePrivateProfileStringW(L"Settings", L"Threshold", L"50", cfg);
    Check(WaitOvl(TRUE, 8000), "config.ini Threshold=50 picked up live (no restart) -> overlay @40%");
    WritePrivateProfileStringW(L"Settings", L"Opacity", L"100", cfg);
    Check(WaitAlpha(255, 8000), "config.ini Opacity=100 picked up live (alpha 255)");
    WritePrivateProfileStringW(L"Settings", L"Opacity", L"5", cfg);  /* out of range -> clamped to 10% */
    Check(WaitAlpha(25, 8000), "Opacity=5 clamped to 10% (alpha 25)");
    WritePrivateProfileStringW(L"Settings", L"Threshold", L"garbage", cfg);  /* invalid -> default 20 */
    Check(WaitOvl(FALSE, 8000), "invalid Threshold text falls back to default 20 (no crash)");
    WritePrivateProfileStringW(L"Settings", L"Threshold", L"20", cfg);
    WritePrivateProfileStringW(L"Settings", L"Opacity", L"55", cfg);
    Check(GetForegroundWindow() == app, "FOCUS: unchanged through install + config changes");

    DWORD wOld = WorkerPid(), sOld = SupervisorPid(IEXE);
    Check(RunExe(L"--silent", 40000, EXE), "re-running installer (upgrade in place)");
    for (int i = 0; i < 200 && (!WorkerPid() || WorkerPid() == wOld); i++) Sleep(50);
    Check(WaitCount(2, 10000), "after upgrade: still exactly 1 supervisor + 1 worker");
    Check(SupervisorPid(IEXE) != sOld && WorkerPid() != wOld, "after upgrade: fresh processes (old ones replaced)");

    Sim(0, 8, 0, 0);
    Check(WaitOvl(TRUE, 8000), "installed copy: unplugged @8% -> red");
    Check(RunExe(L"--uninstall --silent", 40000, EXE), "uninstaller ran");
    Check(WaitCount(0, 8000), "uninstall: all processes gone");
    Check(WaitNoOvl(3000), "uninstall: overlay gone");
    Check(WaitGone(inst, 5000), "uninstall: exe deleted");
    WCHAR d[MAX_PATH]; _snwprintf(d, MAX_PATH - 1, L"%ls\\LowBatteryRed", la);
    Check(WaitGone(d, 5000), "uninstall: install folder removed");
    rl = sizeof rv;
    Check(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"LowBatteryRed", RRF_RT_REG_SZ, NULL, rv, &rl) != ERROR_SUCCESS,
          "uninstall: Run key removed");
    Check(GetForegroundWindow() == app, "FOCUS: unchanged for the whole test run");

    DestroyWindow(app);
    printf("\n==== RESULT: %d passed, %d failed ====\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
