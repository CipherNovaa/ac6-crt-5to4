#include <windows.h>
// Stands in as start_protected_game.exe in CRT/offline mode: launches ModEngine2
// targeting AC6 with our mod folder, which then starts armoredcore6.exe (EAC-disabled,
// menu/*.gfx redirected to mod/). The real game still auto-loads our dxgi.dll hook.
int WINAPI WinMain(HINSTANCE h, HINSTANCE hp, LPSTR cmd, int show) {
    char line[] = "me2_launcher.exe -t ac6 -c config_armoredcore6.toml -p armoredcore6.exe --modengine-dll modengine2\\bin\\modengine2.dll";
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si)); si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));
    if (!CreateProcessA(NULL, line, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi))
        return 1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    return (int)code;
}
