/*
 * Snapshot 系统安装工具 - 绿色单文件 Win32 GUI
 * 用于 PE 环境下安装 SNA 格式系统镜像
 * 支持 UEFI 引导修复
 */

#define _WIN32_IE 0x0600
#define _WIN32_WINNT 0x0601
#define UNICODE
#define _UNICODE
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlwapi.h>
#include <stdio.h>
#include <string.h>
#include <process.h>

#include "resource.h"

HINSTANCE g_hInst;
HWND g_hWnd, g_hSnaPath, g_hTargetDrive, g_hPassword, g_hStatus, g_hDriveList;
HWND g_hBtnInstall, g_hChkFixBoot;

typedef struct {
    WCHAR szDrive[4];
    WCHAR szLabel[256];
    WCHAR szFS[32];
    ULARGE_INTEGER totalBytes, freeBytes;
    UINT uDriveType;
    BOOL bValid;
} DRIVE_INFO;

#define MAX_DRIVES 26
DRIVE_INFO g_drives[MAX_DRIVES];
int g_driveCount = 0;

/* ====== Helper Functions ====== */

static const WCHAR* GetDriveTypeStr(UINT type) {
    switch (type) {
        case DRIVE_FIXED:    return L"本地磁盘";
        case DRIVE_REMOVABLE: return L"可移动磁盘";
        case DRIVE_CDROM:    return L"光驱";
        case DRIVE_RAMDISK:  return L"虚拟内存盘";
        default:             return L"未知";
    }
}

static void FormatBytes(WCHAR* buf, size_t size, ULARGE_INTEGER bytes) {
    double d = (double)bytes.QuadPart;
    const WCHAR* unit[] = { L"B", L"KB", L"MB", L"GB", L"TB" };
    int i = 0;
    while (d >= 1024.0 && i < 4) { d /= 1024.0; i++; }
    if (i == 0) swprintf(buf, 32, L"%.0f %s", d, unit[i]);
    else swprintf(buf, 32, L"%.1f %s", d, unit[i]);
}

/* Run bcdboot to fix UEFI boot */
static BOOL FixUEFIBoot(const WCHAR* systemDrive) {
    WCHAR cmdLine[1024];

    SetWindowTextW(g_hStatus, L"正在修复 UEFI 引导...");

    /* Simple bcdboot - auto-detect ESP, Chinese locale */
    swprintf(cmdLine, 1024, L"bcdboot %s\\Windows /l zh-cn", systemDrive);

    PROCESS_INFORMATION pi = {0};
    STARTUPINFOW si = {0};
    si.cb = sizeof(si);

    BOOL ret = CreateProcessW(NULL, cmdLine, NULL, NULL, FALSE,
                              NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi);
    if (!ret) return FALSE;

    WaitForSingleObject(pi.hProcess, 30000);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    return (exitCode == 0);
}


static void RefreshDriveList(void) {
    WCHAR szRoot[8], display[512], sizeStr[32], freeStr[32];
    DWORD dwDrives = GetLogicalDrives();
    g_driveCount = 0;

    SendMessage(g_hTargetDrive, CB_RESETCONTENT, 0, 0);
    ListView_DeleteAllItems(g_hDriveList);

    for (int i = 0; i < 26; i++) {
        if (!(dwDrives & (1 << i))) continue;
        swprintf(szRoot, 8, L"%c:\\", L'A' + i);
        UINT dt = GetDriveTypeW(szRoot);
        if (dt != DRIVE_FIXED && dt != DRIVE_REMOVABLE) continue;

        DRIVE_INFO* di = &g_drives[g_driveCount];
        memset(di, 0, sizeof(DRIVE_INFO));
        swprintf(di->szDrive, 4, L"%c:", L'A' + i);
        wcscpy(di->szLabel, L"");
        wcscpy(di->szFS, L"");
        di->uDriveType = dt;

        GetVolumeInformationW(szRoot, di->szLabel, 256, NULL, NULL, NULL, di->szFS, 32);
        GetDiskFreeSpaceExW(szRoot, &di->freeBytes, &di->totalBytes, NULL);

        if (di->totalBytes.QuadPart == 0) continue;
        di->bValid = TRUE;

        FormatBytes(sizeStr, 32, di->totalBytes);
        FormatBytes(freeStr, 32, di->freeBytes);
        swprintf(display, 512, L"%s - %s (%s 可用)", di->szDrive, sizeStr, freeStr);

        int idx = (int)SendMessageW(g_hTargetDrive, CB_ADDSTRING, 0, (LPARAM)display);
        SendMessage(g_hTargetDrive, CB_SETITEMDATA, (WPARAM)idx, (LPARAM)i);

        LVITEMW lvi = { LVIF_TEXT, g_driveCount, 0, 0, 0, di->szDrive };
        lvi.iItem = g_driveCount;
        ListView_InsertItem(g_hDriveList, &lvi);
        ListView_SetItemText(g_hDriveList, g_driveCount, 1, sizeStr);
        ListView_SetItemText(g_hDriveList, g_driveCount, 2, freeStr);
        ListView_SetItemText(g_hDriveList, g_driveCount, 3, di->szFS);
        ListView_SetItemText(g_hDriveList, g_driveCount, 4, GetDriveTypeStr(di->uDriveType));
        ListView_SetItemText(g_hDriveList, g_driveCount, 5, di->szLabel);

        g_driveCount++;
    }
    if (g_hTargetDrive && g_driveCount > 0)
        SendMessage(g_hTargetDrive, CB_SETCURSEL, 0, 0);
}

/* ====== File Open Dialog ====== */

static BOOL OpenSnaFile(HWND hwnd, WCHAR* path, int pathSize) {
    OPENFILENAMEW ofn = {0};
    WCHAR fileBuf[MAX_PATH] = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrFilter = L"SNA 镜像文件\0*.sna\0所有文件\0*.*\0";
    ofn.nFilterIndex = 1;
    ofn.lpstrTitle = L"选择 SNA 系统镜像文件";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
    ofn.lpstrDefExt = L"sna";
    if (GetOpenFileNameW(&ofn)) { wcscpy(path, fileBuf); return TRUE; }
    return FALSE;
}

/* ====== Install Thread ====== */

typedef struct {
    WCHAR* cmdLine;
    WCHAR targetDrive[4];
    BOOL fixBoot;
} INSTALL_CTX;

static unsigned int __stdcall InstallThread(void* param) {
    INSTALL_CTX* ctx = (INSTALL_CTX*)param;
    WCHAR snapshotExe[MAX_PATH];
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    DWORD exitCode = 0;

    GetModuleFileNameW(NULL, snapshotExe, MAX_PATH);
    WCHAR* p = wcsrchr(snapshotExe, L'\\');
    if (p) *(p + 1) = 0;
    wcscat(snapshotExe, L"Snapshot64.exe");

    if (GetFileAttributesW(snapshotExe) == INVALID_FILE_ATTRIBUTES) {
        SetWindowTextW(g_hStatus, L"错误：未找到 Snapshot64.exe，请放到程序同目录");
        EnableWindow(g_hBtnInstall, TRUE);
        free(ctx->cmdLine); free(ctx); return 1;
    }

    /* --- Step 1: Run Snapshot --- */
    SetWindowTextW(g_hStatus, L"正在执行系统还原，请耐心等待...");
    EnableWindow(g_hBtnInstall, FALSE);

    si.cb = sizeof(si);

    /* Set working directory to same as Snapshot64.exe */
    WCHAR workDir[MAX_PATH];
    wcscpy(workDir, snapshotExe);
    WCHAR* pW = wcsrchr(workDir, L'\\');
    if (pW) *pW = 0;

    BOOL ret = CreateProcessW(NULL, ctx->cmdLine, NULL, NULL, FALSE,
                              NORMAL_PRIORITY_CLASS, NULL, workDir, &si, &pi);
    if (!ret) {
        WCHAR errBuf[128];
        swprintf(errBuf, 128, L"启动 Snapshot64.exe 失败 (%d)", GetLastError());
        SetWindowTextW(g_hStatus, errBuf);
        EnableWindow(g_hBtnInstall, TRUE);
        free(ctx->cmdLine); free(ctx); return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);

    if (exitCode != 0) {
        WCHAR msg[256];
        swprintf(msg, 256, L"安装失败，Snapshot64 返回错误码: %d", exitCode);
        SetWindowTextW(g_hStatus, msg);
        EnableWindow(g_hBtnInstall, TRUE);
        free(ctx->cmdLine); free(ctx); return 1;
    }

    /* --- Step 2: Fix UEFI Boot (if checked) --- */
    if (ctx->fixBoot) {
        SetWindowTextW(g_hStatus, L"系统还原完成！正在修复 UEFI 引导...");
        BOOL bootOk = FixUEFIBoot(ctx->targetDrive);

        if (bootOk) {
            SetWindowTextW(g_hStatus, L"系统安装完成，UEFI 引导已修复！请重启计算机。");
            MessageBoxW(g_hWnd,
                L"✅ 系统安装成功完成！\n✅ UEFI 引导已修复！\n\n请关闭此程序并重启计算机。",
                L"安装成功", MB_OK | MB_ICONINFORMATION);
        } else {
            WCHAR errMsg[512];
            swprintf(errMsg, 512,
                L"⚠️ 系统安装完成，但 UEFI 引导修复失败。\n\n"
                L"请手动运行以下命令修复引导：\nbcdboot %s\\Windows /f UEFI",
                ctx->targetDrive);
            SetWindowTextW(g_hStatus, L"系统安装完成，但 UEFI 引导修复失败，请手动修复。");
            MessageBoxW(g_hWnd, errMsg, L"引导修复失败", MB_OK | MB_ICONWARNING);
        }
    } else {
        SetWindowTextW(g_hStatus, L"系统安装完成！请重启计算机。");
        MessageBoxW(g_hWnd, L"系统安装成功完成！\n请关闭此程序并重启计算机。",
                    L"安装成功", MB_OK | MB_ICONINFORMATION);
    }

    EnableWindow(g_hBtnInstall, TRUE);
    free(ctx->cmdLine); free(ctx);
    return 0;
}

/* ====== Start Install ====== */

static void StartInstall(HWND hwnd) {
    WCHAR snaPath[MAX_PATH] = {0}, password[64] = {0};
    int sel;
    BOOL fixBoot;

    /* Get SNA path from edit box */
    GetWindowTextW(g_hSnaPath, snaPath, MAX_PATH);
    if (wcslen(snaPath) == 0) {
        MessageBoxW(hwnd, L"请先选择 SNA 镜像文件！", L"提示", MB_OK | MB_ICONWARNING);
        return;
    }

    sel = (int)SendMessage(g_hTargetDrive, CB_GETCURSEL, 0, 0);
    if (sel == CB_ERR) {
        MessageBoxW(hwnd, L"请选择目标分区！", L"提示", MB_OK | MB_ICONWARNING); return;
    }
    GetWindowTextW(g_hPassword, password, 64);
    fixBoot = (SendMessage(g_hChkFixBoot, BM_GETCHECK, 0, 0) == BST_CHECKED);

    int driveIdx = (int)SendMessage(g_hTargetDrive, CB_GETITEMDATA, sel, 0);
    WCHAR targetDrive[4];
    swprintf(targetDrive, 4, L"%c:", L'A' + driveIdx);

    WCHAR confirmMsg[1024];
    swprintf(confirmMsg, 1024,
        L"请确认安装信息：\n\n镜像文件: %s\n目标分区: %s\n密码: %s\n修复引导: %s\n\n"
        L"⚠ 目标分区上的所有数据将被覆盖！请确认已备份。",
        snaPath, targetDrive, password[0] ? password : L"(未设置)",
        fixBoot ? L"是 ✓" : L"否");

    if (MessageBoxW(hwnd, confirmMsg, L"确认安装", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
        return;

    WCHAR selfPath[MAX_PATH] = {0}, exeDir[MAX_PATH] = {0};
    GetModuleFileNameW(NULL, selfPath, MAX_PATH);
    wcscpy(exeDir, selfPath);
    WCHAR* pS = wcsrchr(exeDir, L'\\');
    if (pS) *(pS + 1) = 0;
    wcscat(exeDir, L"Snapshot64.exe");

    WCHAR* cmdLine = (WCHAR*)malloc(2048 * sizeof(WCHAR));
    if (!cmdLine) return;

    if (wcslen(password) > 0)
        swprintf(cmdLine, 2048, L"\"%s\" \"%s\" %s -PW=%s -Go -Y",
                 exeDir, snaPath, targetDrive, password);
    else
        swprintf(cmdLine, 2048, L"\"%s\" \"%s\" %s -Go -Y",
                 exeDir, snaPath, targetDrive);

    INSTALL_CTX* ctx = (INSTALL_CTX*)malloc(sizeof(INSTALL_CTX));
    if (!ctx) { free(cmdLine); return; }
    ctx->cmdLine = cmdLine;
    wcscpy(ctx->targetDrive, targetDrive);
    ctx->fixBoot = fixBoot;

    unsigned int tid;
    CloseHandle((HANDLE)_beginthreadex(NULL, 0, InstallThread, ctx, 0, &tid));
}

/* ====== Window Procedure ====== */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_CREATE: {
            HFONT hFont = CreateFontW(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");

            /* Title */
            CreateWindowW(L"STATIC", L"Snapshot 系统安装工具",
                WS_VISIBLE | WS_CHILD | SS_CENTER | SS_CENTERIMAGE,
                50, 10, 500, 32, hwnd, NULL, g_hInst, NULL);

            /* Image file - Combo box + browse button */
            CreateWindowW(L"STATIC", L"镜像文件 (.sna):", WS_VISIBLE | WS_CHILD,
                         20, 55, 130, 22, hwnd, NULL, g_hInst, NULL);
            g_hSnaPath = CreateWindowW(L"EDIT", L"",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_READONLY,
                150, 53, 315, 24, hwnd, (HMENU)IDC_SNA_PATH, g_hInst, NULL);
            CreateWindowW(L"BUTTON", L"浏览...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          470, 52, 75, 25, hwnd, (HMENU)IDC_BTN_BROWSE, g_hInst, NULL);

            /* Target drive */
            CreateWindowW(L"STATIC", L"目标分区:", WS_VISIBLE | WS_CHILD,
                         20, 93, 90, 22, hwnd, NULL, g_hInst, NULL);
            g_hTargetDrive = CreateWindowW(WC_COMBOBOXW, L"",
                WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
                110, 91, 200, 200, hwnd, (HMENU)IDC_TARGET_DRIVE, g_hInst, NULL);

            /* Password */
            CreateWindowW(L"STATIC", L"还原密码:", WS_VISIBLE | WS_CHILD,
                         325, 93, 70, 22, hwnd, NULL, g_hInst, NULL);
            g_hPassword = CreateWindowW(L"EDIT", L"",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
                395, 91, 145, 24, hwnd, (HMENU)IDC_PASSWORD, g_hInst, NULL);

            /* Fix UEFI checkbox */
            g_hChkFixBoot = CreateWindowW(L"BUTTON", L"安装完成后修复 UEFI 引导",
                WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX | BS_LEFT,
                20, 127, 280, 22, hwnd, (HMENU)IDC_CHK_FIXBOOT, g_hInst, NULL);
            SendMessage(g_hChkFixBoot, BM_SETCHECK, BST_CHECKED, 0);

            /* Drive list */
            g_hDriveList = CreateWindowW(WC_LISTVIEWW, L"",
                WS_VISIBLE | WS_CHILD | WS_BORDER |
                LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER,
                20, 155, 525, 140, hwnd, (HMENU)IDC_DRIVE_LIST, g_hInst, NULL);

            WCHAR* titles[] = { L"盘符", L"总大小", L"可用空间", L"文件系统", L"类型", L"卷标" };
            int widths[] = { 50, 80, 80, 65, 75, 150 };
            for (int i = 0; i < 6; i++) {
                LVCOLUMNW lvc = { LVCF_TEXT | LVCF_WIDTH, 0, widths[i], titles[i] };
                ListView_InsertColumn(g_hDriveList, i, &lvc);
            }
            ListView_SetExtendedListViewStyle(g_hDriveList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            /* Install button */
            g_hBtnInstall = CreateWindowW(L"BUTTON", L"开始安装系统",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                115, 308, 330, 38, hwnd, (HMENU)IDC_BTN_INSTALL, g_hInst, NULL);

            /* Status */
            g_hStatus = CreateWindowW(L"EDIT", L"就绪，等待操作...",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_READONLY,
                20, 358, 520, 28, hwnd, (HMENU)IDC_STATUS, g_hInst, NULL);

            /* Apply font */
            if (hFont) {
                HWND child = NULL;
                while ((child = FindWindowExW(hwnd, child, NULL, NULL)) != NULL)
                    SendMessage(child, WM_SETFONT, (WPARAM)hFont, TRUE);
            }

            /* Initial data */
            RefreshDriveList();
            break;
        }

        case WM_COMMAND: {
            if (LOWORD(wParam) == IDC_BTN_BROWSE) {
                WCHAR path[MAX_PATH] = {0};
                if (OpenSnaFile(hwnd, path, MAX_PATH)) {
                    SetWindowTextW(g_hSnaPath, path);
                    SetWindowTextW(g_hStatus, L"已选择镜像文件");
                }
            } else if (LOWORD(wParam) == IDC_BTN_INSTALL) {
                StartInstall(hwnd);
            }
            break;
        }

        case WM_DESTROY: PostQuitMessage(0); break;
        default: return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return 0;
}

/* ====== Entry Point ====== */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    g_hInst = hInstance;
    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_WIN95_CLASSES | ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc = {
        sizeof(wc), CS_HREDRAW | CS_VREDRAW, WndProc,
        0, 0, hInstance,
        LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP)),
        LoadCursor(NULL, IDC_ARROW),
        (HBRUSH)(COLOR_WINDOW + 1),
        NULL, L"SnapInstClass",
        LoadIconW(hInstance, MAKEINTRESOURCEW(IDI_APP))
    };
    if (!RegisterClassExW(&wc)) return 1;

    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    int wx = 570, wy = 410;
    int x = (sw - wx) / 2, y = (sh - wy) / 2;

    g_hWnd = CreateWindowExW(0, L"SnapInstClass", L"Snapshot 系统安装工具",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        x, y, wx, wy, NULL, NULL, hInstance, NULL);
    if (!g_hWnd) return 1;

    ShowWindow(g_hWnd, nCmdShow); UpdateWindow(g_hWnd);
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) { TranslateMessage(&msg); DispatchMessage(&msg); }
    return (int)msg.wParam;
}
