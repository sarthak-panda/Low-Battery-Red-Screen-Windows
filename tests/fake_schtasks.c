/* Test double for schtasks.exe: logs its command line, captures the /XML file. Pure Win32. */
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <wchar.h>
static void Append(const WCHAR *path, const void *data, DWORD n)
{
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w; WriteFile(f, data, n, &w, NULL); CloseHandle(f);
}
int wmain(void)
{
    const WCHAR *cl = GetCommandLineW();
    Append(L"C:\\schtasks_log.txt", cl, (DWORD)(wcslen(cl) * 2));
    Append(L"C:\\schtasks_log.txt", L"\r\0\n\0", 8);
    const WCHAR *x = wcsstr(cl, L"/XML \"");
    if (!x) return 0;
    WCHAR path[MAX_PATH]; int i = 0; x += 6;
    while (*x && *x != L'"' && i < MAX_PATH - 1) path[i++] = *x++;
    path[i] = 0;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    static BYTE buf[65536]; DWORD n = 0; ReadFile(f, buf, sizeof buf, &n, NULL); CloseHandle(f);
    WCHAR out[32] = L"C:\\task_0.xml";
    for (int k = 1; k < 9; k++) { out[8] = (WCHAR)(L'0' + k); if (GetFileAttributesW(out) == INVALID_FILE_ATTRIBUTES) break; }
    HANDLE o = CreateFileW(out, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL);
    if (o != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(o, buf, n, &w, NULL); CloseHandle(o); }
    WCHAR tmp[8];
    if (GetEnvironmentVariableW(L"FAKE_FAIL_LOGON", tmp, 8))       /* pretend the OS rejects LogonTrigger */
        for (DWORD j = 0; j + 28 <= n; j += 2)
            if (!memcmp(buf + j, L"<LogonTrigger>", 28)) return 1;
    return 0;
}
