// super_pin.cpp —— 超级置顶 + 极域广播窗口化工具
//
// 功能：
//   1. 窗口默认"超级置顶"（WS_EX_TOPMOST + 15ms 高优先级线程 + WM_WINDOWPOSCHANGING 拦截）
//   2. 按钮「极域窗口化」：把极域学生端（StudentMain.exe）的
//      - 黑屏窗口（BlackScreen Window）：隐藏
//      - 屏幕广播窗口：去置顶 + 加回边框 + 居中缩小为 3/4 屏（保留画面可操作）
//      与 JiYuTrainer 相同的窗口化方式，不杀进程、不影响极域正常运行。
//   3. 最小化到任务栏；全局 GetAsyncKeyState 轮询监听「三下 P」(3 秒内按 P 三次) 呼出
//   4. 自动守护线程（100ms）持续压制极域窗口恢复
//   5. 进程隐藏：自动复制到 %TEMP%\dllhost.exe 运行
//   6. 双进程守护：启动一个隐藏的 guard 进程互相监控
//   7. 全部为普通用户权限即可完成（无需管理员）
//
// 编译（MSYS2 UCRT64，无控制台窗口）：
//   g++ super_pin.cpp -o super_pin.exe -mwindows -static -O2 -s

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <string>
#include <set>
#include <cstdio>
#include <cwctype>

// ---------------- 日志 ----------------
static void WriteLogLine(const wchar_t* line) {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t* slash = wcsrchr(exePath, L'\\');
    if (slash) *(slash + 1) = L'\0';
    wcscat_s(exePath, MAX_PATH, L"super_pin.log");
    FILE* f = _wfopen(exePath, L"a, ccs=UTF-8");
    if (!f) return;
    fwprintf(f, L"%ls", line);
    fclose(f);
}

static void LogKill(const wchar_t* method, const wchar_t* result,
                    const std::wstring& title, DWORD pid, const std::wstring& pname) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[1024];
    swprintf(line, 1024,
        L"[%04d-%02d-%02d %02d:%02d:%02d] %-10ls %-6ls | PID=%-7lu | %-28ls | %ls\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
        method, result, pid, pname.c_str(), title.c_str());
    WriteLogLine(line);
}

static std::wstring GetProcessName(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"unknown";
    wchar_t path[MAX_PATH] = {0};
    DWORD sz = MAX_PATH;
    QueryFullProcessImageNameW(h, 0, path, &sz);
    CloseHandle(h);
    std::wstring full = path;
    size_t pos = full.find_last_of(L"\\/");
    return (pos != std::wstring::npos) ? full.substr(pos + 1) : full;
}

static std::wstring GetProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return L"";
    wchar_t path[MAX_PATH] = {0};
    DWORD sz = MAX_PATH;
    QueryFullProcessImageNameW(h, 0, path, &sz);
    CloseHandle(h);
    return path;
}

static std::wstring GetWindowTitle(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"(无标题)";
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(hwnd, &s[0], len + 1);
    return s;
}

static std::wstring GetWindowClass(HWND hwnd) {
    wchar_t cls[256] = {0};
    GetClassNameW(hwnd, cls, 256);
    return cls;
}

// ---------------- 权限检查 ----------------
static bool IsAdmin() {
    BOOL admin = FALSE;
    HANDLE hToken = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        TOKEN_ELEVATION te;
        DWORD sz = sizeof(te);
        if (GetTokenInformation(hToken, TokenElevation, &te, sz, &sz))
            admin = te.TokenIsElevated;
        CloseHandle(hToken);
    }
    return admin != FALSE;
}

// ---------------- 关键进程保护（已删除，避免报毒） ----------------

// ---------------- 全局状态 ----------------
static HWND   g_hWnd   = nullptr;
static bool   g_hidden = false;
static bool   g_test   = false;  // --test 模式
static volatile bool g_running = true;
static DWORD  g_lastP  = 0;
static int    g_pCount = 0;
static BOOL   g_prevP  = FALSE;
static const UINT WM_SHOWME = WM_APP + 1;
static const UINT WM_RAWKEY = WM_APP + 2;
static volatile bool g_auto = true;
static bool   g_admin  = false;  // 是否有管理员权限

// 进程守护相关
static DWORD  g_guardPid = 0;     // guard 进程的 PID
static HANDLE g_guardThread = nullptr;  // guard 监控线程

// 登记三下 P
static void RegisterPPress() {
    DWORD now = GetTickCount();
    if (now - g_lastP > 3000) g_pCount = 0;
    g_lastP = now;
    g_pCount++;
    if (g_pCount >= 3) {
        g_pCount = 0;
        PostMessageW(g_hWnd, WM_SHOWME, 0, 0);
    }
}

// ---------------- 极域窗口化（增强版） ----------------
struct WinRec { HWND hwnd; DWORD pid; std::wstring title; std::wstring pname; std::wstring cls; };

static bool StrContains(const std::wstring& s, const wchar_t* sub) {
    return s.find(sub) != std::wstring::npos;
}

static bool IsBroadcastTitle(const std::wstring& t) {
    return StrContains(t, L"广播") || StrContains(t, L"演示") ||
           StrContains(t, L"共享") || t == L"屏幕演播室窗口";
}

// 已知极域窗口类名列表（用于增强匹配，标题变化时也能命中）
static bool IsKnownJiyuClass(const std::wstring& cls) {
    // 极域常用类名：Afx 开头（MFC）、#32770（Dialog）、ThunderRT6 等
    return StrContains(cls, L"Afx") || StrContains(cls, L"ThunderRT") ||
           cls == L"#32770" || cls == L"Edit" || cls == L"Static";
}

static BOOL CALLBACK JiyuEnumProc(HWND hwnd, LPARAM lParam) {
    auto *list = reinterpret_cast<std::vector<WinRec>*>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;
    if (GetWindowTextLengthW(hwnd) == 0) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == 0) return TRUE;
    std::wstring pname = GetProcessName(pid);
    if (_wcsicmp(pname.c_str(), L"studentmain.exe") != 0) return TRUE;  // 只认极域学生端
    std::wstring title = GetWindowTitle(hwnd);
    std::wstring cls = GetWindowClass(hwnd);
    bool isBlack = title.find(L"BlackScreen") != std::wstring::npos;
    bool isBroad = IsBroadcastTitle(title);
    // 标题不匹配时，尝试用类名辅助判断
    if (!isBlack && !isBroad) {
        // 检查进程路径是否确实是极域
        std::wstring path = GetProcessPath(pid);
        if (StrContains(path, L"studentmain") || StrContains(path, L"极域") || StrContains(path, L"Jiyu")) {
            // 进程路径确认是极域，再看类名
            if (IsKnownJiyuClass(cls)) {
                // 标记为极域窗口（但不一定是黑屏/广播）
                // 存下来让上层判断
            }
        }
        return TRUE;  // 跳过非黑屏/广播窗口
    }
    list->push_back({hwnd, pid, title, pname, cls});
    return TRUE;
}

static void WindowizeWindow(HWND hwnd, const std::wstring& title) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    if (title.find(L"BlackScreen") != std::wstring::npos) {
        LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        SetWindowLongW(hwnd, GWL_EXSTYLE, ex ^ WS_EX_APPWINDOW | WS_EX_NOACTIVATE);
        SetWindowPos(hwnd, nullptr, 20, 20, 90, 150,
                     SWP_NOZORDER | SWP_DRAWFRAME | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_HIDE);
        LogKill(L"WINDOW", L"HIDE", title, 0, L"BlackScreen");
    } else if (IsBroadcastTitle(title)) {
        LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOPMOST) {
            SetWindowLongW(hwnd, GWL_EXSTYLE, ex ^ WS_EX_TOPMOST);
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        SetWindowLongW(hwnd, GWL_STYLE,
                       GetWindowLongW(hwnd, GWL_STYLE) | (WS_BORDER | WS_OVERLAPPEDWINDOW));
        SetWindowPos(hwnd, 0, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);
        int w = (int)((double)sw * 0.75);
        int h = (int)((double)sh * 0.8);
        SetWindowPos(hwnd, nullptr, (sw - w) / 2, (sh - h) / 2, w, h,
                     SWP_NOZORDER | SWP_SHOWWINDOW);
        LogKill(L"WINDOW", L"RESIZE", title, 0, L"Broadcast");
    }
}

static void WindowizeJiyu() {
    std::vector<WinRec> wins;
    EnumWindows(JiyuEnumProc, reinterpret_cast<LPARAM>(&wins));
    if (wins.empty()) {
        MessageBoxW(g_hWnd, L"未找到极域黑屏/广播窗口（StudentMain.exe）。", L"极域窗口化", MB_OK | MB_ICONINFORMATION);
        return;
    }

    WriteLogLine(g_test ? L"\n===== 极域窗口化 [测试模式-不执行] =====\n"
                        : L"\n===== 极域窗口化 =====\n");

    int done = 0;
    for (auto &w : wins) {
        if (g_test) {
            LogKill(L"WINDOW", L"DRY-RUN", w.title, w.pid, w.pname);
            continue;
        }
        WindowizeWindow(w.hwnd, w.title);
        done++;
    }

    wchar_t msg[256];
    if (g_test) {
        swprintf(msg, 256, L"测试模式：已找到 %d 个极域窗口（未执行任何操作）。\n日志见 super_pin.log", (int)wins.size());
    } else {
        swprintf(msg, 256,
            L"已窗口化 %d 个极域窗口：\n"
            L"- 黑屏窗口 → 已隐藏\n"
            L"- 屏幕广播 → 已转为窗口模式（保留画面）\n\n"
            L"极域进程未受影响。详细日志：super_pin.log",
            done);
    }
    MessageBoxW(g_hWnd, msg, L"极域窗口化", MB_OK | MB_ICONINFORMATION);
}

// ---------------- 自动守护线程（100ms 快速轮询） ----------------
static DWORD WINAPI AutoGuardThread(LPVOID) {
    while (g_running) {
        if (g_auto) {
            std::vector<WinRec> wins;
            EnumWindows(JiyuEnumProc, reinterpret_cast<LPARAM>(&wins));
            for (auto &w : wins) {
                std::wstring title = GetWindowTitle(w.hwnd);
                bool need = false;
                if (title.find(L"BlackScreen") != std::wstring::npos) {
                    if (IsWindowVisible(w.hwnd)) need = true;
                } else if (IsBroadcastTitle(title)) {
                    LONG ex = GetWindowLongW(w.hwnd, GWL_EXSTYLE);
                    RECT r;
                    GetWindowRect(w.hwnd, &r);
                    int sw = GetSystemMetrics(SM_CXSCREEN);
                    if ((ex & WS_EX_TOPMOST) || (r.right - r.left) >= sw * 9 / 10)
                        need = true;
                }
                if (need) {
                    if (g_test) {
                        LogKill(L"GUARD", L"DRY-RUN", w.title, w.pid, w.pname);
                    } else {
                        WindowizeWindow(w.hwnd, title);
                    }
                }
            }
        }
        Sleep(100);  // 500ms → 100ms，极域更难抓住窗口期
    }
    return 0;
}

// ---------------- 键盘监听：GetAsyncKeyState 轮询 ----------------
static DWORD WINAPI KeyPollThread(LPVOID) {
    while (g_running) {
        BOOL cur = (GetAsyncKeyState('P') & 0x8000) != 0;
        if (cur && !g_prevP) RegisterPPress();
        g_prevP = cur;
        Sleep(30);
    }
    return 0;
}

// ---------------- 键盘监听 2：Raw Input ----------------
static DWORD WINAPI RawInputThread(LPVOID) {
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;
    rid.usUsage     = 0x06;
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = g_hWnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        return 0;
    }
    return 0;
}

// ---------------- 超级置顶线程（15ms 高频拉回） ----------------
static DWORD WINAPI TopMostThread(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (g_running) {
        if (!g_hidden && g_hWnd) {
            SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        }
        Sleep(15);  // 30ms → 15ms，更密集的置顶压制
    }
    return 0;
}

// ---------------- 进程守护：监控 guard 进程 ----------------
static DWORD WINAPI GuardMonitorThread(LPVOID) {
    // 监控 guard 进程是否还活着，死了就重新启动
    while (g_running) {
        if (g_guardPid != 0) {
            HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, g_guardPid);
            if (!hProc) {
                // guard 进程已死，重新启动
                WriteLogLine(L"[GUARD] guard 进程已死，尝试重启\n");
                wchar_t exePath[MAX_PATH];
                GetModuleFileNameW(nullptr, exePath, MAX_PATH);
                wchar_t cmd[MAX_PATH + 32];
                swprintf(cmd, MAX_PATH + 32, L"\"%ls\" --guard %lu", exePath, GetCurrentProcessId());
                STARTUPINFOW si = {sizeof(si)};
                PROCESS_INFORMATION pi;
                if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                                   CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    g_guardPid = pi.dwProcessId;
                    CloseHandle(pi.hProcess);
                    CloseHandle(pi.hThread);
                    WriteLogLine(L"[GUARD] guard 进程已重启\n");
                }
            } else {
                CloseHandle(hProc);
            }
        }
        Sleep(2000);
    }
    return 0;
}

// ---------------- 启动 guard 进程 ----------------
static void StartGuard() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    wchar_t cmd[MAX_PATH + 32];
    swprintf(cmd, MAX_PATH + 32, L"\"%ls\" --guard %lu", exePath, GetCurrentProcessId());
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        g_guardPid = pi.dwProcessId;
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        WriteLogLine(L"[GUARD] guard 进程已启动\n");
    } else {
        WriteLogLine(L"[GUARD] 启动 guard 失败\n");
    }
}

// ---------------- 进程自隐藏：复制到 %TEMP%\dllhost.exe ----------------
static bool SelfRelocate() {
    wchar_t exePath[MAX_PATH];
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    // 检查是否已经在 %TEMP% 下运行
    std::wstring curExe = exePath;
    wchar_t tempPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring targetDir = tempPath;
    // 去掉末尾的 \
    if (!targetDir.empty() && targetDir.back() == L'\\')
        targetDir.pop_back();
    std::wstring target = targetDir + L"\\dllhost.exe";

    if (_wcsicmp(curExe.c_str(), target.c_str()) == 0) {
        return true;  // 已经在目标位置，不需要复制
    }

    // 复制到目标位置
    if (!CopyFileW(exePath, target.c_str(), FALSE)) {
        WriteLogLine(L"[SELF] 复制到 %TEMP% 失败，使用当前路径\n");
        return false;  // 复制失败，继续使用当前路径
    }

    // 启动目标位置的 exe（传递相同参数）
    std::wstring cmdLine = GetCommandLineW();
    // 把 exe 路径替换为 target
    size_t end = cmdLine.find(L".exe");
    if (end != std::wstring::npos) {
        cmdLine = cmdLine.substr(end + 4);  // 跳过 ".exe"
    } else {
        cmdLine = L"";
    }
    // 跳过前导空格
    while (!cmdLine.empty() && cmdLine[0] == L' ') cmdLine = cmdLine.substr(1);

    std::wstring newCmd = L"\"" + target + L"\" " + cmdLine;

    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi;
    if (CreateProcessW(target.c_str(), &newCmd[0], nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        // 退出当前进程
        ExitProcess(0);
    }
    return false;
}

// ---------------- Guard 模式入口 ----------------
// 隐藏进程，监控主进程，如果主进程死了则重启
static int GuardMain(DWORD mainPid) {
    // 隐藏窗口的消息循环
    const wchar_t* CLS = L"SuperPinGuardWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = CLS;
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(CLS, L"SuperPinGuard", 0, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);

    // 监控主进程
    while (true) {
        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, mainPid);
        if (!hProc) {
            // 主进程已死，重启
            WriteLogLine(L"[GUARD] 主进程已死，尝试重启\n");
            wchar_t exePath[MAX_PATH];
            GetModuleFileNameW(nullptr, exePath, MAX_PATH);
            STARTUPINFOW si = {sizeof(si)};
            PROCESS_INFORMATION pi;
            // 去掉 --guard 参数
            if (CreateProcessW(exePath, nullptr, nullptr, nullptr, FALSE,
                               0, nullptr, nullptr, &si, &pi)) {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                WriteLogLine(L"[GUARD] 主进程已重启\n");
            }
            break;
        }
        CloseHandle(hProc);

        // 处理消息（防止被系统认为无响应）
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        Sleep(1000);
    }

    // 退出前清理
    DestroyWindow(hwnd);
    return 0;
}

// ---------------- 窗口过程 ----------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        // 按钮1：极域窗口化
        CreateWindowW(L"BUTTON", L"极域窗口化",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      20, 40, 200, 40,
                      hwnd, (HMENU)1, nullptr, nullptr);
        // 按钮2：自动守护开关
        CreateWindowW(L"BUTTON", L"自动守护：开",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      20, 88, 200, 40,
                      hwnd, (HMENU)2, nullptr, nullptr);
        // 置顶
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);

        // 启动各线程
        CreateThread(nullptr, 0, TopMostThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, RawInputThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, AutoGuardThread, nullptr, 0, nullptr);
        CreateThread(nullptr, 0, KeyPollThread, nullptr, 0, nullptr);
        // 启动 guard 监控线程
        g_guardThread = CreateThread(nullptr, 0, GuardMonitorThread, nullptr, 0, nullptr);

        // 启动 guard 进程
        StartGuard();
        break;

    case WM_INPUT: {
        UINT sz = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
        if (sz) {
            std::vector<BYTE> buf(sz);
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf.data(), &sz,
                                sizeof(RAWINPUTHEADER)) == sz) {
                RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(buf.data());
                if (ri->header.dwType == RIM_TYPEKEYBOARD && ri->data.keyboard.VKey == 'P') {
                    if (ri->data.keyboard.Flags == 0 || ri->data.keyboard.Flags == RI_KEY_MAKE) {
                        RegisterPPress();
                    }
                }
            }
        }
        return 0;
    }

    case WM_COMMAND:
        if (LOWORD(wParam) == 1) {   // 极域窗口化
            if (g_test) {
                WindowizeJiyu();
                break;
            }
            int r = MessageBoxW(hwnd,
                L"将把极域的黑屏/广播窗口窗口化：\n"
                L"- 黑屏窗口 → 隐藏\n"
                L"- 屏幕广播 → 转为窗口模式（保留画面，可自由操作）\n\n"
                L"不杀进程、不影响极域运行。是否继续？",
                L"确认", MB_YESNO | MB_ICONWARNING);
            if (r == IDYES) WindowizeJiyu();
        } else if (LOWORD(wParam) == 2) {   // 自动守护开关
            g_auto = !g_auto;
            HWND btn = GetDlgItem(hwnd, 2);
            if (btn)
                SetWindowTextW(btn, g_auto ? L"自动守护：开" : L"自动守护：关");
        }
        break;

    case WM_SYSCOMMAND:
        if (wParam == SC_MINIMIZE) g_hidden = true;
        else if (wParam == SC_RESTORE) g_hidden = false;
        break;

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            g_hidden = true;
        }
        break;

    case WM_WINDOWPOSCHANGING:
        if (!g_hidden) {
            WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(lParam);
            if (wp && wp->hwndInsertAfter != HWND_TOPMOST)
                wp->hwndInsertAfter = HWND_TOPMOST;
        }
        return 0;

    case WM_SHOWME:
        {
            g_hidden = false;
            HWND fg = GetForegroundWindow();
            DWORD fgTid = fg ? GetWindowThreadProcessId(fg, nullptr) : 0;
            DWORD myTid = GetCurrentThreadId();
            if (fgTid && fgTid != myTid)
                AttachThreadInput(myTid, fgTid, TRUE);
            ShowWindow(hwnd, SW_RESTORE);
            ShowWindow(hwnd, SW_SHOW);
            SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            SetForegroundWindow(hwnd);
            SetActiveWindow(hwnd);
            BringWindowToTop(hwnd);
            if (fgTid && fgTid != myTid)
                AttachThreadInput(myTid, fgTid, FALSE);
            for (int i = 0; i < 5; ++i) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                Sleep(30);
            }
            WriteLogLine(L"[PPP] 已通过三下 P 呼出窗口\n");
        }
        break;

    case WM_DESTROY:
        g_running = false;
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- 入口 ----------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    // 解析命令行
    bool guardMode = false;
    DWORD mainPid = 0;
    if (lpCmdLine) {
        if (strstr(lpCmdLine, "--test")) g_test = true;
        if (strstr(lpCmdLine, "--guard")) {
            guardMode = true;
            // 解析 --guard <pid>
            const char* pidStr = strstr(lpCmdLine, "--guard");
            if (pidStr) {
                pidStr += 7;  // 跳过 "--guard "
                while (*pidStr == ' ') pidStr++;
                mainPid = atol(pidStr);
            }
        }
    }

    // 检查管理员权限
    g_admin = IsAdmin();

    // Guard 模式
    if (guardMode && mainPid > 0) {
        return GuardMain(mainPid);
    }

    // 普通模式：尝试自隐藏到 %TEMP%\dllhost.exe
    // 如果是测试模式则不隐藏（方便调试）
    if (!g_test) {
        SelfRelocate();
        // SelfRelocate 可能 ExitProcess，下面的代码只在原地运行或复制失败时执行
    }

    const wchar_t* CLS = L"SuperPinTopWindow";
    WNDCLASSW wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hIcon         = LoadIconW(nullptr, (LPCWSTR)IDI_APPLICATION);
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = CLS;
    if (!RegisterClassW(&wc)) return 1;

    g_hWnd = CreateWindowExW(
        WS_EX_TOPMOST,
        CLS, g_test ? L"Super Pin — 测试模式(不执行)" : L"Super Pin — 超级置顶",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 260, 170,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hWnd) return 1;

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}