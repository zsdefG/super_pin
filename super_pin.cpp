// super_pin.cpp —— 超级置顶 + 极域广播窗口化工具
//
// 功能：
//   1. 窗口默认"超级置顶"（WS_EX_TOPMOST + 30ms 高优先级线程 + WM_WINDOWPOSCHANGING 拦截）
//   2. 按钮「极域窗口化」：把极域学生端（StudentMain.exe）的
//      - 黑屏窗口（BlackScreen Window）：隐藏
//      - 屏幕广播窗口：去置顶 + 加回边框 + 居中缩小为 3/4 屏（保留画面可操作）
//      与 JiYuTrainer 相同的窗口化方式，不杀进程、不影响极域正常运行。
//   3. 最小化到任务栏；全局 GetAsyncKeyState 轮询监听「三下 P」(3 秒内按 P 三次) 呼出
//   4. 全部为普通用户权限即可完成（无需管理员）
//
// 编译（MSYS2 UCRT64，无控制台窗口）：
//   g++ super_pin.cpp -o super_pin.exe -mwindows -static -O2

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <vector>
#include <string>
#include <set>
#include <cstdio>
#include <cwctype>

// ---------------- 日志 ----------------
// 每次「极域窗口化」操作把窗口信息写入 exe 同目录 super_pin.log。
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

static std::wstring GetWindowTitle(HWND hwnd) {
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return L"(无标题)";
    std::wstring s(len + 1, L'\0');
    GetWindowTextW(hwnd, &s[0], len + 1);
    return s;
}

// ---------------- 全局状态 ----------------
static HWND   g_hWnd   = nullptr;
static bool   g_hidden = false;
static bool   g_test   = false;  // --test 模式：只枚举+写日志，不真正操作窗口
static volatile bool g_running = true;  // 轮询线程运行标志
static DWORD  g_lastP  = 0;   // 上次按 P 的时间
static int    g_pCount = 0;   // 连续按 P 的次数（3 秒窗口内）
static BOOL   g_prevP  = FALSE;  // GetAsyncKeyState 上次 P 键状态（检测按下沿）
static const UINT WM_SHOWME = WM_APP + 1;  // 自定义消息：呼出窗口
static const UINT WM_RAWKEY = WM_APP + 2;  // Raw Input 收到按键（跨线程通知）
static volatile bool g_auto = true;        // 自动守护开关（默认开）

// 登记三下 P：带锁保证线程安全
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

// ---------------- 极域窗口化（仿 JiYuTrainer） ----------------
// 只处理 StudentMain.exe 的窗口，不杀进程、不影响极域运行。
struct WinRec { HWND hwnd; DWORD pid; std::wstring title; std::wstring pname; };

static bool StrContains(const std::wstring& s, const wchar_t* sub) {
    return s.find(sub) != std::wstring::npos;
}

// 广播窗口标题特征（JiYuTrainer 同款匹配）：含"广播/演示/共享"或等于"屏幕演播室窗口"
static bool IsBroadcastTitle(const std::wstring& t) {
    return StrContains(t, L"广播") || StrContains(t, L"演示") ||
           StrContains(t, L"共享") || t == L"屏幕演播室窗口";
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
    bool isBlack = title.find(L"BlackScreen") != std::wstring::npos;
    bool isBroad = IsBroadcastTitle(title);
    if (!isBlack && !isBroad) return TRUE;
    list->push_back({hwnd, pid, title, pname});
    return TRUE;
}

// 把极域黑屏/广播窗口"窗口化"（JiYuTrainer 的 FakeFull(false) 逻辑）：
//   - 黑屏窗口：缩到角落并隐藏
//   - 广播窗口：去掉 TOPMOST、加回边框、居中缩为 3/4 屏宽 4/5 屏高，保留画面
static void WindowizeWindow(HWND hwnd, const std::wstring& title) {
    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);

    if (title.find(L"BlackScreen") != std::wstring::npos) {
        // 黑屏窗口：缩小丢角落 + 隐藏
        LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        SetWindowLongW(hwnd, GWL_EXSTYLE, ex ^ WS_EX_APPWINDOW | WS_EX_NOACTIVATE);
        SetWindowPos(hwnd, nullptr, 20, 20, 90, 150,
                     SWP_NOZORDER | SWP_DRAWFRAME | SWP_NOACTIVATE);
        ShowWindow(hwnd, SW_HIDE);
        LogKill(L"WINDOW", L"HIDE", title, 0, L"BlackScreen");
    } else if (IsBroadcastTitle(title)) {
        // 广播窗口：去置顶 + 加边框 + 居中 3/4 屏
        LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
        if (ex & WS_EX_TOPMOST) {
            SetWindowLongW(hwnd, GWL_EXSTYLE, ex ^ WS_EX_TOPMOST);
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
        SetWindowLongW(hwnd, GWL_STYLE,
                       GetWindowLongW(hwnd, GWL_STYLE) | (WS_BORDER | WS_OVERLAPPEDWINDOW));
        SetWindowPos(hwnd, 0, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_FRAMECHANGED);  // 应用新样式（含可调边框）
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

// ---------------- 自动守护线程 ----------------
// 极域广播可能把键盘完全锁死（BlockInput/驱动级过滤），PPP 按键通道物理上失效。
// 守护线程每 500ms 扫描一次：只要极域黑屏/广播窗口出现（或极域每 1s 拉回全屏置顶），
// 就自动再把它窗口化——不依赖任何按键，形成持续压制，让广播始终变不回全屏。
// 需要判断"已窗口化"避免空转：黑屏已隐藏=已处理；广播窗口无 TOPMOST 且 <90% 屏=已处理。
static DWORD WINAPI AutoGuardThread(LPVOID) {
    while (g_running) {
        if (g_auto) {
            std::vector<WinRec> wins;
            EnumWindows(JiyuEnumProc, reinterpret_cast<LPARAM>(&wins));
            for (auto &w : wins) {
                std::wstring title = GetWindowTitle(w.hwnd);
                bool need = false;
                if (title.find(L"BlackScreen") != std::wstring::npos) {
                    // 黑屏：只要还可见就再隐藏（SW_HIDE 后 IsWindowVisible=False，自动跳过）
                    if (IsWindowVisible(w.hwnd)) need = true;
                } else if (IsBroadcastTitle(title)) {
                    // 广播：还带 TOPMOST 或尺寸 ≥90% 屏 = 极域又拉回全屏，需要再窗口化
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
        Sleep(500);
    }
    return 0;
}

// ---------------- 键盘监听：GetAsyncKeyState 轮询（抗钩子劫持） ----------------
// 极域用 WH_KEYBOARD_LL 低级钩子拦截按键，我们的钩子会排在它后面收不到事件。
// GetAsyncKeyState 直接读物理键盘状态表，绕过低级钩子链，极域屏蔽无效。
static DWORD WINAPI KeyPollThread(LPVOID) {
    while (g_running) {
        BOOL cur = (GetAsyncKeyState('P') & 0x8000) != 0;
        if (cur && !g_prevP) RegisterPPress();   // 新一次按下
        g_prevP = cur;
        Sleep(30);
    }
    return 0;
}

// ---------------- 键盘监听 2：Raw Input（物理 HID 输入队列，最底层） ----------------
// 极域若用 BlockInput 或驱动级键盘过滤，GetAsyncKeyState 可能失效；
// RegisterRawInputDevices 注册"后台接收"（RIDEV_INPUTSINK）直接从 HID 队列拿按键，
// 是用户态能触达的最底层键盘通道。
static DWORD WINAPI RawInputThread(LPVOID) {
    // 注册键盘为后台输入设备（窗口无需前台也能收到）
    RAWINPUTDEVICE rid = {};
    rid.usUsagePage = 0x01;      // Generic Desktop
    rid.usUsage     = 0x06;      // Keyboard
    rid.dwFlags     = RIDEV_INPUTSINK;   // 即使非前台也接收
    rid.hwndTarget  = g_hWnd;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        return 0;  // 注册失败（权限等），依赖 GetAsyncKeyState 通道
    }
    return 0;  // 注册成功后由 WndProc 的 WM_INPUT 处理
}

// ---------------- 超级置顶线程（高优先级高频拉回） ----------------
// 极域黑屏每 ~1s SetWindowPos 拉回一次；本线程 30ms 拉回一次，
// 并配合 WM_WINDOWPOSCHANGING 拦截，让窗口始终钉在最顶、不闪现。
static DWORD WINAPI TopMostThread(LPVOID) {
    // 用户是管理员，可尝试 TIME_CRITICAL；失败降级也无妨
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    while (g_running) {
        if (!g_hidden && g_hWnd) {
            SetWindowPos(g_hWnd, HWND_TOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
        }
        Sleep(30);
    }
    return 0;
}

// ---------------- 窗口过程 ----------------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        // 按钮1：极域窗口化（黑屏隐藏 + 广播转窗口，仿 JiYuTrainer）
        CreateWindowW(L"BUTTON", L"极域窗口化",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      20, 40, 200, 40,
                      hwnd, (HMENU)1, nullptr, nullptr);
        // 按钮2：自动守护开关（默认开）
        CreateWindowW(L"BUTTON", L"自动守护：开",
                      WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                      20, 88, 200, 40,
                      hwnd, (HMENU)2, nullptr, nullptr);
        // 置顶
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
        // 高优先级置顶线程：30ms 高频拉回，压过极域 1s 拉回，不闪现
        CreateThread(nullptr, 0, TopMostThread, nullptr, 0, nullptr);
        // Raw Input 键盘通道：物理 HID 队列，抗 BlockInput/驱动级过滤
        CreateThread(nullptr, 0, RawInputThread, nullptr, 0, nullptr);
        // 自动守护线程：不依赖按键，发现极域控制窗口自动窗口化
        CreateThread(nullptr, 0, AutoGuardThread, nullptr, 0, nullptr);
        break;

    case WM_INPUT: {
        // Raw Input 键盘事件（从 HID 队列直接来，绕过 WH_KEYBOARD_LL 钩子链）
        UINT sz = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &sz, sizeof(RAWINPUTHEADER));
        if (sz) {
            std::vector<BYTE> buf(sz);
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf.data(), &sz,
                                sizeof(RAWINPUTHEADER)) == sz) {
                RAWINPUT* ri = reinterpret_cast<RAWINPUT*>(buf.data());
                if (ri->header.dwType == RIM_TYPEKEYBOARD && ri->data.keyboard.VKey == 'P') {
                    if (ri->data.keyboard.Flags == 0 || ri->data.keyboard.Flags == RI_KEY_MAKE) {
                        RegisterPPress();   // 按下沿，计三连击
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
        // 最小化/恢复前同步 g_hidden，避免 WM_WINDOWPOSCHANGING 拦截最小化
        if (wParam == SC_MINIMIZE) g_hidden = true;
        else if (wParam == SC_RESTORE) g_hidden = false;
        break;  // 交回 DefWindowProc 处理

    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) {
            // 最小化到任务栏（任务栏有按钮，点击可恢复）；PPP 也可呼出
            g_hidden = true;
        }
        break;

    case WM_WINDOWPOSCHANGING:
        // 超级置顶核心：任何把本窗口移出最顶的请求，强制改回 HWND_TOPMOST。
        // 极域 SetWindowPos(黑屏窗口, TOPMOST) 时会触发本窗口的
        // WM_WINDOWPOSCHANGING，这里立刻把 hwndInsertAfter 改回 TOPMOST，
        // 使黑屏窗口无法真正压到本窗口之上。
        if (!g_hidden) {
            WINDOWPOS* wp = reinterpret_cast<WINDOWPOS*>(lParam);
            if (wp && wp->hwndInsertAfter != HWND_TOPMOST)
                wp->hwndInsertAfter = HWND_TOPMOST;
        }
        return 0;

    case WM_TIMER:
        break;   // 置顶改由 TopMostThread 线程负责

    case WM_SHOWME:
        // 三下 P 呼出（最小化/隐藏后）。
        // 极域广播时前台窗口归极域控制，SetForegroundWindow 会被前台锁拒绝，
        // 因此用 AttachThreadInput 强制夺取前台 + 立即置顶 + BringWindowToTop。
        {
            g_hidden = false;   // 先解除隐藏，让置顶线程/拦截立即接管

            // 强制激活：附加到前台线程后 SetForegroundWindow
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

            // 兜底：连拉几次置顶，确保压过极域广播窗口
            for (int i = 0; i < 5; ++i) {
                SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                Sleep(30);
            }
            WriteLogLine(L"[PPP] 已通过三下 P 呼出窗口\n");
        }
        break;

    case WM_DESTROY:
        g_running = false;   // 停止轮询线程 + 置顶线程
        PostQuitMessage(0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------- 入口 ----------------
int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    // --test：测试模式，按钮只枚举+写日志，不真正关闭/强杀任何窗口
    if (lpCmdLine && strstr(lpCmdLine, "--test")) g_test = true;

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
        WS_EX_TOPMOST,                     // 超级置顶；任务栏显示按钮，可最小化/恢复
        CLS, g_test ? L"Super Pin — 测试模式(不执行)" : L"Super Pin — 超级置顶",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT, 260, 170,
        nullptr, nullptr, hInst, nullptr);
    if (!g_hWnd) return 1;

    // 键盘轮询线程：GetAsyncKeyState 读物理键盘，抗低级钩子劫持
    CreateThread(nullptr, 0, KeyPollThread, nullptr, 0, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
