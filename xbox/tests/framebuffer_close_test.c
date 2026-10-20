#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void xbox_FramebufferWindowStart(void);
ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }
static DWORD child_pid;
static HWND child_window;
static BOOL CALLBACK find_window(HWND window, LPARAM unused)
{
    DWORD pid;
    char name[64];
    (void)unused;
    GetWindowThreadProcessId(window, &pid);
    GetClassNameA(window, name, sizeof(name));
    if (pid == child_pid && !strcmp(name, "XboxRecompFramebuffer"))
        child_window = window;
    return TRUE;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        _putenv("RECOMP_FB_WINDOW=1");
        /* The executor tries to start the window on every clear. */
        for (;;) { xbox_FramebufferWindowStart(); Sleep(10); }
    }
    char path[MAX_PATH], command[MAX_PATH + 32];
    STARTUPINFOA si = { sizeof(si) };
    PROCESS_INFORMATION pi;
    GetModuleFileNameA(NULL, path, sizeof(path));
    snprintf(command, sizeof(command), "\"%s\" child", path);
    if (!CreateProcessA(NULL, command, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 2;
    child_pid = pi.dwProcessId;
    DWORD deadline = GetTickCount() + 5000;
    while (!child_window && (LONG)(deadline - GetTickCount()) > 0) {
        EnumWindows(find_window, 0);
        Sleep(10);
    }
    if (child_window) PostMessageA(child_window, WM_CLOSE, 0, 0);
    DWORD wait = WaitForSingleObject(pi.hProcess, 2000), code = 1;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 99);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    if (!child_window || wait != WAIT_OBJECT_0 || code != 0) {
        fprintf(stderr, "Closing the framebuffer must exit the host (wait=%lu, exit=%lu)\n", wait, code);
        return 1;
    }
    puts("Framebuffer close exits the host without reopening.");
    return 0;
}
