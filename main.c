/*
 * Snapshot 系统安装工具 - 绿色单文件 Win32 GUI
 * 用于 PE 环境下安装 SNA 格式系统镜像
 * 支持 UEFI 引导修复
 * v1.1: 新增全盘自动搜索 SNA 镜像功能
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
HWND g_hWnd, g_hSnaCombo, g_hTargetDrive, g_hPassword, g_hStatus, g_hDriveList;
HWND g_hBtnInstall, g_hChkFixBoot, g_hSearchStatus;

/* 自动搜索 SNA 列表 */
#define MAX_SNA_FILES 128
WCHAR g_snaFiles[MAX_SNA_FILES][MAX_PATH];
int   g_snaCount = 0;
BOOL  g_bSearching = FALSE;

typedef struct {
    HWND hwnd;
} SEARCH_CTX;

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

        LVITEMW lvi = { LVIF_TEXT, g_driveCount, 0, 0, 0, (LPWSTR)di->szDrive };
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

/* ====== SNA 文件搜索 ====== */

/* 递归搜索指定根目录下的所有 .sna 文件 */
static void SearchSnaInDir(const WCHAR* dir) {
    if (g_snaCount >= MAX_SNA_FILES) return;

    WCHAR pattern[MAX_PATH];
    swprintf(pattern, MAX_PATH, L"%s\\*", dir);

    WIN32_FIND_DATAW fd;
    HANDLE hFind = FindFirstFileW(pattern, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    do {
        if (g_snaCount >= MAX_SNA_FILES) break;
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;

        WCHAR fullPath[MAX_PATH];
        swprintf(fullPath, MAX_PATH, L"%s\\%s", dir, fd.cFileName);

        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            /* 跳过系统/隐藏目录以加快速度 */
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM))
                SearchSnaInDir(fullPath);
        } else {
            /* 检查 .sna 后缀（不区分大小写） */
            WCHAR* ext = wcsrchr(fd.cFileName, L'.');
            if (ext && _wcsicmp(ext, L".sna") == 0) {
                wcscpy(g_snaFiles[g_snaCount], fullPath);
                g_snaCount++;
            }
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

/* 搜索线程：遍历所有逻辑磁盘 */
static unsigned int __stdcall SearchThread(void* param) {
    HWND hwnd = (HWND)param;
    g_bSearching = TRUE;
    g_snaCount = 0;

    /* 更新搜索状态 */
    SetWindowTextW(g_hSearchStatus, L"正在搜索 SNA 镜像，请稍候...");

    DWORD dwDrives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(dwDrives & (1 << i))) continue;
        WCHAR szRoot[4];
        swprintf(szRoot, 4, L"%c:\\", L'A' + i);
        UINT dt = GetDriveTypeW(szRoot);
        /* 搜索固定磁盘和可移动磁盘 */
        if (dt == DRIVE_FIXED || dt == DRIVE_REMOVABLE || dt == DRIVE_RAMDISK) {
            WCHAR msg[64];
            swprintf(msg, 64, L"正在搜索 %c 盘...", L'A' + i);
            SetWindowTextW(g_hSearchStatus, msg);
            WCHAR searchDir[4];
            swprintf(searchDir, 4, L"%c:", L'A' + i);
            SearchSnaInDir(szRoot);
        }
    }

    /* 搜索结束，将结果发送到主窗口 */
    PostMessageW(hwnd, WM_APP + 1, 0, 0);
    g_bSearching = FALSE;
    return 0;
}

/* 将搜索结果填入下拉列表 */
static void PopulateSnaCombo(void) {
    SendMessageW(g_hSnaCombo, CB_RESETCONTENT, 0, 0);

    if (g_snaCount == 0) {
        SendMessageW(g_hSnaCombo, CB_ADDSTRING, 0, (LPARAM)L"（未找到 SNA 镜像，请点击浏览选择）");
        SendMessageW(g_hSnaCombo, CB_SETCURSEL, 0, 0);
        WCHAR msg[64];
        swprintf(msg, 64, L"搜索完成，未找到 SNA 镜像文件");
        SetWindowTextW(g_hSearchStatus, msg);
    } else {
        for (int i = 0; i < g_snaCount; i++) {
            SendMessageW(g_hSnaCombo, CB_ADDSTRING, 0, (LPARAM)g_snaFiles[i]);
        }
        SendMessageW(g_hSnaCombo, CB_SETCURSEL, 0, 0);
        WCHAR msg[64];
        swprintf(msg, 64, L"搜索完成，找到 %d 个 SNA 镜像", g_snaCount);
        SetWindowTextW(g_hSearchStatus, msg);
    }
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

    /* 从下拉列表获取当前选中的 SNA 路径 */
    GetWindowTextW(g_hSnaCombo, snaPath, MAX_PATH);
    /* 去掉首尾空格后判空 */
    {
        WCHAR* p = snaPath;
        while (*p == L' ') p++;
        if (wcslen(p) == 0 || p[0] == L'\xef' /* 占位符 */ ||
            wcscmp(p, L"（未找到 SNA 镜像，请点击浏览选择）") == 0) {
            MessageBoxW(hwnd, L"请先选择或浏览 SNA 镜像文件！", L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
        /* 检查文件扩展名 */
        WCHAR* ext = wcsrchr(p, L'.');
        if (!ext || _wcsicmp(ext, L".sna") != 0) {
            MessageBoxW(hwnd, L"请选择有效的 SNA 镜像文件！", L"提示", MB_OK | MB_ICONWARNING);
            return;
        }
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

            /* ---- SNA 镜像文件选择区 ---- */
            /* 标签 */
            CreateWindowW(L"STATIC", L"镜像文件 (.sna):", WS_VISIBLE | WS_CHILD,
                         20, 52, 130, 22, hwnd, NULL, g_hInst, NULL);

            /* 下拉列表（可编辑，支持手动输入路径） */
            g_hSnaCombo = CreateWindowW(WC_COMBOBOXW, L"",
                WS_VISIBLE | WS_CHILD | CBS_DROPDOWN | CBS_AUTOHSCROLL | WS_VSCROLL,
                150, 50, 310, 260, hwnd, (HMENU)IDC_SNA_COMBO, g_hInst, NULL);

            /* 浏览按钮 */
            CreateWindowW(L"BUTTON", L"浏览...", WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                          466, 50, 80, 25, hwnd, (HMENU)IDC_BTN_BROWSE, g_hInst, NULL);

            /* 搜索状态 */
            g_hSearchStatus = CreateWindowW(L"STATIC", L"正在准备搜索...",
                WS_VISIBLE | WS_CHILD | SS_LEFT,
                150, 79, 396, 18, hwnd, (HMENU)IDC_SEARCH_STATUS, g_hInst, NULL);

            /* ---- 目标分区 ---- */
            CreateWindowW(L"STATIC", L"目标分区:", WS_VISIBLE | WS_CHILD,
                         20, 108, 90, 22, hwnd, NULL, g_hInst, NULL);
            g_hTargetDrive = CreateWindowW(WC_COMBOBOXW, L"",
                WS_VISIBLE | WS_CHILD | CBS_DROPDOWNLIST | WS_VSCROLL,
                110, 106, 200, 200, hwnd, (HMENU)IDC_TARGET_DRIVE, g_hInst, NULL);

            /* ---- 还原密码 ---- */
            CreateWindowW(L"STATIC", L"还原密码:", WS_VISIBLE | WS_CHILD,
                         325, 108, 70, 22, hwnd, NULL, g_hInst, NULL);
            g_hPassword = CreateWindowW(L"EDIT", L"",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_AUTOHSCROLL | ES_PASSWORD,
                395, 106, 151, 24, hwnd, (HMENU)IDC_PASSWORD, g_hInst, NULL);

            /* ---- 修复 UEFI 引导 ---- */
            g_hChkFixBoot = CreateWindowW(L"BUTTON", L"安装完成后修复 UEFI 引导",
                WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX | BS_LEFT,
                20, 142, 280, 22, hwnd, (HMENU)IDC_CHK_FIXBOOT, g_hInst, NULL);
            SendMessage(g_hChkFixBoot, BM_SETCHECK, BST_CHECKED, 0);

            /* ---- 磁盘列表 ---- */
            g_hDriveList = CreateWindowW(WC_LISTVIEWW, L"",
                WS_VISIBLE | WS_CHILD | WS_BORDER |
                LVS_REPORT | LVS_SINGLESEL | LVS_NOSORTHEADER,
                20, 172, 525, 140, hwnd, (HMENU)IDC_DRIVE_LIST, g_hInst, NULL);

            WCHAR* titles[] = { L"盘符", L"总大小", L"可用空间", L"文件系统", L"类型", L"卷标" };
            int widths[] = { 50, 80, 80, 65, 75, 150 };
            for (int i = 0; i < 6; i++) {
                LVCOLUMNW lvc = { LVCF_TEXT | LVCF_WIDTH, 0, widths[i], titles[i] };
                ListView_InsertColumn(g_hDriveList, i, &lvc);
            }
            ListView_SetExtendedListViewStyle(g_hDriveList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

            /* ---- 安装按钮 ---- */
            g_hBtnInstall = CreateWindowW(L"BUTTON", L"开始安装系统",
                WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
                115, 325, 330, 38, hwnd, (HMENU)IDC_BTN_INSTALL, g_hInst, NULL);

            /* ---- 状态栏 ---- */
            g_hStatus = CreateWindowW(L"EDIT", L"就绪，等待操作...",
                WS_VISIBLE | WS_CHILD | WS_BORDER | ES_READONLY,
                20, 375, 520, 28, hwnd, (HMENU)IDC_STATUS, g_hInst, NULL);

            /* 应用字体 */
            if (hFont) {
                HWND child = NULL;
                while ((child = FindWindowExW(hwnd, child, NULL, NULL)) != NULL)
                    SendMessage(child, WM_SETFONT, (WPARAM)hFont, TRUE);
            }

            /* 初始化磁盘列表 */
            RefreshDriveList();

            /* 启动后台搜索线程 */
            unsigned int tid;
            CloseHandle((HANDLE)_beginthreadex(NULL, 0, SearchThread, (void*)hwnd, 0, &tid));
            break;
        }

        /* 搜索线程完成，填充下拉列表 */
        case WM_APP + 1: {
            PopulateSnaCombo();
            break;
        }

        case WM_COMMAND: {
            WORD id = LOWORD(wParam);
            if (id == IDC_BTN_BROWSE) {
                /* 浏览选择 SNA 文件 */
                WCHAR path[MAX_PATH] = {0};
                if (OpenSnaFile(hwnd, path, MAX_PATH)) {
                    /* 检查是否已在列表中，没有则追加 */
                    int found = (int)SendMessageW(g_hSnaCombo, CB_FINDSTRINGEXACT, (WPARAM)-1, (LPARAM)path);
                    if (found == CB_ERR) {
                        found = (int)SendMessageW(g_hSnaCombo, CB_ADDSTRING, 0, (LPARAM)path);
                    }
                    SendMessageW(g_hSnaCombo, CB_SETCURSEL, (WPARAM)found, 0);
                    /* 同步编辑框文本（CBS_DROPDOWN 模式下需要手动设置） */
                    SetWindowTextW(g_hSnaCombo, path);
                    SetWindowTextW(g_hStatus, L"已选择镜像文件");
                }
            } else if (id == IDC_BTN_INSTALL) {
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

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    (void)hPrevInstance; (void)lpCmdLine;
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
    int wx = 570, wy = 430;
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
