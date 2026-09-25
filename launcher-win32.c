#include <windows.h>
#include <commdlg.h>
#include <shlobj.h>
#include <stdio.h>
#include <string.h>

#define ID_PORT     101
#define ID_WARM     102
#define ID_BROWSE   103
#define ID_START    104
#define ID_STOP     105
#define ID_STATUS   106
#define ID_DIRLABEL 107

static HWND hPort, hWarm, hBrowse, hStart, hStop, hStatus, hDirLabel;
static char chosen_dir[MAX_PATH];
static HANDLE hChildProcess;
static int child_running;

static void get_core_path(char *buf, int bufsz) {
    GetModuleFileNameA(NULL, buf, bufsz);
    char *last = strrchr(buf, '\\');
    if (last) *(last + 1) = '\0';
    strcat(buf, "minvader.exe");
}

static void update_status(const char *msg) {
    SetWindowTextA(hStatus, msg);
}

static DWORD WINAPI watch_child(LPVOID param) {
    WaitForSingleObject(hChildProcess, INFINITE);
    CloseHandle(hChildProcess);
    hChildProcess = NULL;
    child_running = 0;
    update_status("Server stopped");
    EnableWindow(hStart, TRUE);
    EnableWindow(hStop, FALSE);
    EnableWindow(hPort, TRUE);
    EnableWindow(hWarm, TRUE);
    if (SendMessage(hWarm, BM_GETCHECK, 0, 0) != BST_CHECKED)
        EnableWindow(hBrowse, TRUE);
    return 0;
}

static void do_start(HWND hwnd) {
    char port[64];
    GetWindowTextA(hPort, port, sizeof(port));
    if (!port[0]) { update_status("Error: port is required"); return; }

    int warm = (SendMessage(hWarm, BM_GETCHECK, 0, 0) == BST_CHECKED);
    if (!warm && !chosen_dir[0]) {
        update_status("Error: choose a directory or enable warm start");
        return;
    }

    char binary[MAX_PATH];
    get_core_path(binary, sizeof(binary));

    char cmdline[8192];
    if (warm) {
        snprintf(cmdline, sizeof(cmdline),
            "\"%s\" --warm --port=%s", binary, port);
    } else {
        snprintf(cmdline, sizeof(cmdline),
            "cmd.exe /c \"dir /s /b /a-d \"%s\" | \"%s\" --stdin --port=%s\"",
            chosen_dir, binary, port);
    }

    STARTUPINFOA si = { .cb = sizeof(si) };
    PROCESS_INFORMATION pi = {0};

    if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Launch failed (error %lu)", GetLastError());
        update_status(msg);
        return;
    }

    CloseHandle(pi.hThread);
    hChildProcess = pi.hProcess;
    child_running = 1;

    char msg[256];
    snprintf(msg, sizeof(msg), "Starting on port %s...", port);
    update_status(msg);

    EnableWindow(hStart, FALSE);
    EnableWindow(hStop, TRUE);
    EnableWindow(hPort, FALSE);
    EnableWindow(hWarm, FALSE);
    EnableWindow(hBrowse, FALSE);

    CreateThread(NULL, 0, watch_child, NULL, 0, NULL);
}

static void do_stop(void) {
    if (child_running && hChildProcess)
        TerminateProcess(hChildProcess, 0);
}

static void do_browse(HWND hwnd) {
    BROWSEINFOA bi = {0};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = "Choose a directory to serve";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (pidl) {
        SHGetPathFromIDListA(pidl, chosen_dir);
        SetWindowTextA(hDirLabel, chosen_dir);
        CoTaskMemFree(pidl);
    }
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        CreateWindowA("STATIC", "Port:", WS_CHILD | WS_VISIBLE,
            20, 20, 40, 20, hwnd, NULL, NULL, NULL);
        hPort = CreateWindowA("EDIT", "9090",
            WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            65, 18, 70, 22, hwnd, (HMENU)ID_PORT, NULL, NULL);
        hWarm = CreateWindowA("BUTTON", "Warm start (use existing cache)",
            WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
            155, 20, 250, 20, hwnd, (HMENU)ID_WARM, NULL, NULL);

        CreateWindowA("STATIC", "Directory:", WS_CHILD | WS_VISIBLE,
            20, 60, 65, 20, hwnd, NULL, NULL, NULL);
        hDirLabel = CreateWindowA("STATIC", "(none selected)",
            WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS,
            90, 60, 250, 20, hwnd, (HMENU)ID_DIRLABEL, NULL, NULL);
        hBrowse = CreateWindowA("BUTTON", "Browse...",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            355, 56, 80, 26, hwnd, (HMENU)ID_BROWSE, NULL, NULL);

        hStart = CreateWindowA("BUTTON", "Start",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            130, 100, 80, 28, hwnd, (HMENU)ID_START, NULL, NULL);
        hStop = CreateWindowA("BUTTON", "Stop",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            230, 100, 80, 28, hwnd, (HMENU)ID_STOP, NULL, NULL);
        EnableWindow(hStop, FALSE);

        hStatus = CreateWindowA("STATIC", "Ready", WS_CHILD | WS_VISIBLE,
            20, 145, 420, 20, hwnd, (HMENU)ID_STATUS, NULL, NULL);

        HFONT hFont = CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0,
            DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, "Segoe UI");
        if (hFont) {
            EnumChildWindows(hwnd, (WNDENUMPROC)SendMessageA,
                (LPARAM)WM_SETFONT);
            SendMessageA(hwnd, WM_SETFONT, (WPARAM)hFont, TRUE);
        }
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_WARM: {
            int warm = (SendMessage(hWarm, BM_GETCHECK, 0, 0) == BST_CHECKED);
            EnableWindow(hBrowse, !warm);
            SetWindowTextA(hDirLabel,
                warm ? "(using cache DB)" :
                (chosen_dir[0] ? chosen_dir : "(none selected)"));
            break;
        }
        case ID_BROWSE: do_browse(hwnd); break;
        case ID_START:  do_start(hwnd); break;
        case ID_STOP:   do_stop(); break;
        }
        return 0;
    case WM_CLOSE:
        do_stop();
        if (hChildProcess) {
            WaitForSingleObject(hChildProcess, 3000);
            CloseHandle(hChildProcess);
        }
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE hPrev, LPSTR cmdLine, int nShow) {
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "MinvaderLauncher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA("MinvaderLauncher", "minvader",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 460, 210,
        NULL, NULL, hInst, NULL);

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0)) {
        TranslateMessage(&m);
        DispatchMessage(&m);
    }

    CoUninitialize();
    return (int)m.wParam;
}
