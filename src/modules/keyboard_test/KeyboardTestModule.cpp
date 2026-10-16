#include "KeyboardTestModule.h"
#include "KeyboardUi.h"
#include "modules/ModuleConfig.h"
#include "modules/ModuleCommunication.h"
#include <windowsx.h>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <optional>

namespace ftc::modules::keyboard_test
{
namespace
{
constexpr UINT readyMessage = WM_APP + 0x720, keysMessage = WM_APP + 0x721, stopMessage = WM_APP + 0x722;
constexpr int retestId = 101, endId = 102;
constexpr wchar_t className[] = L"sixunited.KeyboardTest";
struct Session final
{
    explicit Session(ModuleCommunication value) : communication(std::move(value)) {}
    ModuleCommunication communication;
    Language language{Language::Chinese};
    std::filesystem::path path;
    std::atomic<HWND> window{};
    std::mutex mutex;
    std::condition_variable condition;
    bool ready{}, closed{}, explicitClose{};
    unsigned passedKeys{};
    std::size_t requiredKeys{};
    std::optional<TestResult> result;
    KeyboardLayout layout;
    std::wstring error;
};
struct Window final
{
    std::shared_ptr<Session> session;
    std::unique_ptr<KeyboardLogic> logic;
    Canvas canvas;
    HWND hwnd{}, retest{}, end{};
    HFONT font{};
    HHOOK hook{};
    UINT dpi{96}, fixtureDpi{};
    bool useHook{true}, focused{}, scheduled{}, overflow{}, owned{};
    std::array<KeyEvent, 1024> events{};
    std::array<bool, 256> held{}, blocked{};
    std::size_t count{};
    std::wstring current;
};
thread_local Window* activeHook{};
void CleanupHook(Window& window) noexcept
{
    if (activeHook == &window) activeHook = nullptr;
    if (window.hook) { UnhookWindowsHookEx(window.hook); window.hook = nullptr; }
}
void Finish(Window& window, const TestResult result)
{
    const auto state = window.session;
    {
        std::lock_guard lock(state->mutex);
        if (state->result) return;
        if (window.logic)
        {
            state->passedKeys = window.logic->Passed();
            state->requiredKeys = window.logic->Layout().keys.size();
        }
        state->result = result;
        state->explicitClose = true;
    }
    CleanupHook(window);
    DestroyWindow(window.hwnd);
    // DestroyWindow must return before the worker reports the final result.
    { std::lock_guard lock(state->mutex); state->closed = true; }
    state->condition.notify_all();
}
LRESULT CALLBACK KeyHook(const int code, const WPARAM wparam, const LPARAM lparam) noexcept
{
    Window* const window = activeHook;
    if (code < 0 || !window || !window->logic)
        return CallNextHookEx(nullptr, code, wparam, lparam);
    if (code == HC_ACTION && (wparam == WM_KEYDOWN || wparam == WM_KEYUP || wparam == WM_SYSKEYDOWN || wparam == WM_SYSKEYUP))
    {
        const auto& key = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
        const bool blocked = key.vkCode < window->blocked.size() && window->blocked[key.vkCode];
        if (key.vkCode < window->held.size())
        {
            window->held[key.vkCode] = down && key.vkCode != VK_PAUSE;
            if (!down || key.vkCode == VK_PAUSE) window->blocked[key.vkCode] = false;
        }
        if (GetForegroundWindow() != window->hwnd || blocked)
            return CallNextHookEx(nullptr,code,wparam,lparam);
        if (window->count < window->events.size())
            window->events[window->count++] = {{key.scanCode, (key.flags & LLKHF_EXTENDED) != 0, key.vkCode},
                down, GetTickCount64()};
        else window->overflow = true;
        if (!window->scheduled) window->scheduled = PostMessageW(window->hwnd, keysMessage, 0, 0) != FALSE;
        return 1;
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
void Layout(Window& window)
{
    window.dpi = window.fixtureDpi ? window.fixtureDpi : GetDpiForWindow(window.hwnd);
    if (!window.dpi) window.dpi = 96;
    if (window.font) DeleteObject(window.font);
    window.font = CreateFontW(-Px(14,window.dpi), 0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    RECT rect{}; GetClientRect(window.hwnd,&rect);
    const int width = Px(160,window.dpi), height = Px(36,window.dpi), margin = Px(24,window.dpi);
    MoveWindow(window.end, rect.right - margin - width, rect.bottom - margin - height, width, height, TRUE);
    MoveWindow(window.retest, rect.right - margin * 2 - width * 2, rect.bottom - margin - height, width, height, TRUE);
    SendMessageW(window.retest,WM_SETFONT,reinterpret_cast<WPARAM>(window.font),TRUE);
    SendMessageW(window.end,WM_SETFONT,reinterpret_cast<WPARAM>(window.font),TRUE);
    PrepareButtonSurface(window.retest,window.dpi);
    PrepareButtonSurface(window.end,window.dpi);
    InvalidateRect(window.hwnd,nullptr,FALSE);
}
void LoadReady(Window& window)
{
    if (window.logic) return;
    KeyboardLayout layout;
    std::wstring error;
    {
        std::lock_guard lock(window.session->mutex);
        if (!window.session->ready) return;
        error = window.session->error;
        if (error.empty()) layout = window.session->layout;
    }
    if (!error.empty())
    {
        window.session->communication.Print(LogLevel::Error,error);
        Finish(window,TestResult::Fail); return;
    }
    window.logic = std::make_unique<KeyboardLogic>(std::move(layout));
    if (window.useHook)
    {
        if (activeHook)
        {
            window.session->communication.Print(LogLevel::Error,L"Keyboard hook already active");
            Finish(window,TestResult::Fail); return;
        }
        activeHook = &window;
        for (unsigned vk = 0; vk < window.blocked.size(); ++vk)
            window.blocked[vk] = (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
        window.hook = SetWindowsHookExW(WH_KEYBOARD_LL,KeyHook,GetModuleHandleW(nullptr),0);
        if (!window.hook)
        {
            activeHook = nullptr;
            window.session->communication.Print(LogLevel::Error,L"SetWindowsHookEx(WH_KEYBOARD_LL): " + std::to_wstring(GetLastError()));
            Finish(window,TestResult::Fail); return;
        }
    }
    EnableWindow(window.retest,TRUE);
    window.session->communication.SetProgress(0);
    InvalidateRect(window.hwnd,nullptr,FALSE);
}
void Consume(Window& window)
{
    window.scheduled = false;
    if (!window.logic) { window.count = 0; return; }
    if (window.overflow)
    {
        window.logic->Suspend(); window.count = 0; window.overflow = false;
        window.session->communication.Print(LogLevel::Warning,L"Keyboard event queue overflow; release keys and retry");
        InvalidateRect(window.hwnd,nullptr,FALSE); return;
    }
    const auto count = std::exchange(window.count,0);
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto event = window.events[i];
        window.logic->Feed(event);
        window.current = BindingName(event.binding) + L" · " +
            Text(window.session->language,event.down ? L"按下" : L"松开",event.down ? L"Down" : L"Up",event.down ? L"Nhấn" : L"Nhả");
    }
    window.session->communication.SetProgress(window.logic->Passed() * 100U /
        static_cast<unsigned>(window.logic->Layout().keys.size()));
    if (window.logic->Complete()) { Finish(window,TestResult::Pass); return; }
    InvalidateRect(window.hwnd,nullptr,FALSE);
}
void Paint(Window& window)
{
    PAINTSTRUCT paint{}; const HDC dc = BeginPaint(window.hwnd,&paint);
    RECT rect{}; GetClientRect(window.hwnd,&rect);
    if (window.canvas.Begin(window.hwnd,window.dpi))
    {
        const float width = rect.right * 96.0F / window.dpi, height = rect.bottom * 96.0F / window.dpi;
        const auto language = window.session->language;
        window.canvas.Label(Text(language,L"键盘测试",L"Keyboard test",L"Kiểm tra bàn phím"),{24,14,width-24,52},24,600);
        window.canvas.Label(Text(language,L"逐键按下并松开；绿色表示通过，黄色需重试。",L"Press and release each key. Green: passed; amber: retry.",
            L"Nhấn và nhả từng phím. Xanh: đạt; vàng: thử lại."),{24,58,width-24,98},13,400,Design.secondary,false,true);
        const auto count = window.logic ? std::to_wstring(window.logic->Passed()) + L" / " +
            std::to_wstring(window.logic->Layout().keys.size()) : L"—";
        window.canvas.Label(count,{24,108,width-24,146},24,600,fluent::Color::rgb(0x0067C0));
        window.canvas.Label(window.current.empty() ? Text(language,L"等待按键",L"Waiting for a key",L"Đang chờ phím") :
            window.current,{180,108,width-24,146},14);
        if (window.logic)
        {
            const auto geometry = FitKeys(window.logic->Layout(),{24,164,width-24,height-100});
            DrawKeys(window.canvas,window.logic->Layout(),window.logic->Status(),geometry);
        }
        else window.canvas.Label(Text(language,L"正在读取键盘布局…",L"Loading keyboard layout…",L"Đang đọc bố cục bàn phím…"),
            {24,164,width-24,height-100},16,400,Design.secondary,true);
        window.canvas.Label(Text(language,L"未测试",L"Untested",L"Chưa thử"),{24,height-84,130,height-62},12,400,Design.secondary);
        window.canvas.Label(Text(language,L"通过",L"Passed",L"Đạt"),{150,height-84,255,height-62},12,400,fluent::Color::rgb(0x0F7B0F));
        window.canvas.Label(Text(language,L"重试",L"Retry",L"Thử lại"),{275,height-84,375,height-62},12,400,fluent::Color::rgb(0x8A4B00));
        window.canvas.End(window.hwnd);
    }
    else
    {
        FillRect(dc,&rect,GetSysColorBrush(COLOR_WINDOW));
        DrawTextW(dc,L"Keyboard test — press and release configured keys",-1,&rect,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    EndPaint(window.hwnd,&paint);
}
LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept
{
    auto* window = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        window = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        window->hwnd = hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(window));
    }
    if (!window) return DefWindowProcW(hwnd,message,wparam,lparam);
    try
    {
        switch (message)
        {
        case WM_CREATE:
            window->canvas.Attach(hwnd);
            window->retest = CreateWindowW(L"BUTTON",Text(window->session->language,L"重新测试",L"Retest",L"Kiểm tra lại"),
                WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(retestId)),GetModuleHandleW(nullptr),nullptr);
            window->end = CreateWindowW(L"BUTTON",Text(window->session->language,L"结束测试",L"End test",L"Kết thúc kiểm tra"),
                WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,hwnd,reinterpret_cast<HMENU>(static_cast<INT_PTR>(endId)),GetModuleHandleW(nullptr),nullptr);
            if (!window->retest || !window->end || !SetTimer(hwnd,1,50,nullptr)) return -1;
            EnableWindow(window->retest,FALSE); Layout(*window); return 0;
        case WM_ACTIVATE:
            window->focused = LOWORD(wparam) != WA_INACTIVE;
            if (!window->focused)
            {
                window->count = 0; window->overflow = false;
                if (window->logic) window->logic->Suspend();
            }
            else for (unsigned vk = 0; vk < window->blocked.size(); ++vk)
                window->blocked[vk] = window->held[vk] || (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
            window->canvas.Configure(); return 0;
        case WM_SIZE: Layout(*window); return 0;
        case WM_DPICHANGED:
        {
            const auto& r = *reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(hwnd,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
            Layout(*window); return 0;
        }
        case WM_GETMINMAXINFO:
        {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
            limits->ptMinTrackSize = {std::min<long>(Px(680,window->dpi),monitor.rcWork.right-monitor.rcWork.left),
                std::min<long>(Px(480,window->dpi),monitor.rcWork.bottom-monitor.rcWork.top)}; return 0;
        }
        case WM_DRAWITEM: DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam),window->dpi,window->font); return TRUE;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: Paint(*window); return 0;
        case WM_TIMER:
            if (wparam != 1) break;
            if (!window->logic) { LoadReady(*window); return 0; }
            if (window->count || window->overflow) Consume(*window);
            return 0;
        case readyMessage: LoadReady(*window); return 0;
        case keysMessage: Consume(*window); return 0;
        case KeyboardFixtureEvent:
            if (!window->useHook && window->logic)
            {
                const KeyEvent event{{static_cast<unsigned>(lparam & 0xFFFF), (lparam & 0x10000) != 0,
                    static_cast<unsigned>((lparam >> 17) & 0xFF)},wparam != 0,GetTickCount64()};
                window->events[window->count++] = event; Consume(*window);
            }
            return 0;
        case KeyboardFixtureDpi:
            if (!window->useHook && wparam >= 96 && wparam <= 192)
            { window->fixtureDpi = static_cast<UINT>(wparam); Layout(*window); }
            return 0;
        case KeyboardFixtureRetest:
        case WM_COMMAND:
            if (message == KeyboardFixtureRetest || LOWORD(wparam) == retestId)
            {
                if (window->logic)
                {
                    window->count = 0; window->overflow = false; window->logic->Reset(); window->current.clear();
                    window->blocked = window->held;
                    window->session->communication.SetProgress(0); InvalidateRect(hwnd,nullptr,FALSE);
                }
                return 0;
            }
            if (LOWORD(wparam) == endId) { Finish(*window,TestResult::Fail); return 0; }
            break;
        case stopMessage: Finish(*window,TestResult::Cancel); return 0;
        case WM_CLOSE: Finish(*window,TestResult::Fail); return 0;
        case WM_NCDESTROY:
        {
            CleanupHook(*window); if (window->font) DeleteObject(window->font);
            const auto state = window->session;
            const bool owned = window->owned;
            {
                std::lock_guard lock(state->mutex);
                if (!state->result && window->logic)
                {
                    state->passedKeys = window->logic->Passed();
                    state->requiredKeys = window->logic->Layout().keys.size();
                }
            }
            SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);
            const auto result = DefWindowProcW(hwnd,message,wparam,lparam);
            if (owned) delete window;
            state->window.store(nullptr);
            {
                std::lock_guard lock(state->mutex);
                if (!state->explicitClose) state->closed = true;
                if (!state->result) state->result = TestResult::Fail;
            }
            state->condition.notify_all();
            return result;
        }
        }
    }
    catch (...)
    {
        window->session->communication.Print(LogLevel::Error,L"Keyboard window event failed");
        Finish(*window,TestResult::Fail); return 0;
    }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
class KeyboardTestModule final : public ITestModule
{
    std::filesystem::path layout_;
    std::wstring culture_;
    std::shared_ptr<Session> session_;
    std::thread worker_;
    bool show_{true}, useHook_{true}, started_{};
    std::function<void(HWND)> onCreated_;
public:
    KeyboardTestModule(std::filesystem::path layout, std::wstring culture, bool show, bool useHook, std::function<void(HWND)> onCreated)
        : layout_(std::move(layout)),culture_(std::move(culture)),show_(show),useHook_(useHook),onCreated_(std::move(onCreated)) {}
    ~KeyboardTestModule() override
    {
        RequestStop();
        if (session_)
        {
            const HWND window = session_->window.load();
            if (window && GetWindowThreadProcessId(window,nullptr) == GetCurrentThreadId()) SendMessageW(window,stopMessage,0,0);
        }
        if (worker_.joinable()) worker_.join();
    }
    StartOutcome Start(HWND owner, ModuleCallbacks callbacks) override
    {
        if (started_) return StartOutcome::Failure(L"KeyboardTest cannot start twice");
        started_ = true;
        auto state = std::make_shared<Session>(ModuleCommunication(std::move(callbacks)));
        const auto& culture = culture_;
        state->language = _wcsicmp(culture.c_str(),L"en-US") == 0 ? Language::English :
            _wcsicmp(culture.c_str(),L"vi-VN") == 0 ? Language::Vietnamese : Language::Chinese;
        if (layout_.empty()) return StartOutcome::Failure(L"KBL path is empty");
        state->path = layout_;
        WNDCLASSW wc{}; wc.lpfnWndProc = Procedure; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = className; wc.hCursor = LoadCursorW(nullptr,IDC_ARROW);
        wc.hIcon = LoadIconW(GetModuleHandleW(nullptr),MAKEINTRESOURCEW(101));
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            return StartOutcome::Failure(L"RegisterClass(KeyboardTest): " + std::to_wstring(GetLastError()));
        auto window = std::make_unique<Window>(); window->session = state; window->useHook = useHook_;
        const UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
        window->dpi = dpi ? dpi : 96;
        RECT bounds{0,0,Px(1040,window->dpi),Px(580,window->dpi)};
        AdjustWindowRectExForDpi(&bounds,WS_OVERLAPPEDWINDOW,FALSE,0,window->dpi);
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(owner ? MonitorFromWindow(owner,MONITOR_DEFAULTTONEAREST) : MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&monitor);
        const auto width=std::min(bounds.right-bounds.left,monitor.rcWork.right-monitor.rcWork.left);
        const auto height=std::min(bounds.bottom-bounds.top,monitor.rcWork.bottom-monitor.rcWork.top);
        const HWND hwnd = CreateWindowExW(0,className,Text(state->language,L"键盘测试",L"Keyboard test",L"Kiểm tra bàn phím"),
            WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2,
            monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2,width,height,
            owner,nullptr,GetModuleHandleW(nullptr),window.get());
        if (!hwnd) return StartOutcome::Failure(L"CreateWindow(KeyboardTest): " + std::to_wstring(GetLastError()));
        window->owned = true;
        window.release();
        state->window.store(hwnd); session_ = state;
        try
        {
            const ModuleCommunication communication = state->communication;
            worker_ = std::thread([state,communication] {
                KeyboardLayout parsed; std::wstring error;
                bool ok=false;
                try { ok=ReadLayout(state->path,parsed,error); }
                catch (...) { error=L"ReadLayout(KBL): file operation failed"; }
                {
                    std::lock_guard lock(state->mutex);
                    state->layout = std::move(parsed); state->error = ok ? L"" : std::move(error); state->ready = true;
                }
                if (const HWND window = state->window.load()) PostMessageW(window,readyMessage,0,0);
                TestResult result;
                unsigned passedKeys{};
                std::size_t requiredKeys{};
                {
                    std::unique_lock lock(state->mutex);
                    state->condition.wait(lock,[&] { return state->closed; }); result = *state->result;
                    passedKeys = state->passedKeys;
                    requiredKeys = state->requiredKeys;
                }
                std::wstring message = Text(state->language,
                    result == TestResult::Pass ? L"键盘测试通过" : result == TestResult::Fail ? L"键盘测试失败" : L"键盘测试已取消",
                    result == TestResult::Pass ? L"Keyboard test PASS" : result == TestResult::Fail ? L"Keyboard test FAIL" : L"Keyboard test cancelled",
                    result == TestResult::Pass ? L"Kiểm tra bàn phím đạt" : result == TestResult::Fail ? L"Kiểm tra bàn phím thất bại" : L"Đã hủy kiểm tra bàn phím");
                if (requiredKeys != 0)
                    message += std::wstring(Text(state->language,L"；按键通过 ",L"; keys passed ",L"; số phím đạt ")) +
                        std::to_wstring(passedKeys) + L"/" + std::to_wstring(requiredKeys);
                if (result == TestResult::Pass) communication.Pass(message);
                else if (result == TestResult::Fail) communication.Fail(message);
                else communication.Cancel(message);
            });
            if (onCreated_) onCreated_(hwnd);
            if (show_) ShowPrepared(hwnd);
        }
        catch (...)
        {
            SendMessageW(hwnd,stopMessage,0,0);
            if (worker_.joinable()) worker_.join();
            return StartOutcome::Failure(L"KeyboardTest worker creation failed");
        }
        return StartOutcome::Success();
    }
    void RequestStop() noexcept override
    { if (session_) if (const auto hwnd = session_->window.load()) PostMessageW(hwnd,stopMessage,0,0); }
};
}
std::unique_ptr<ITestModule> CreateKeyboardTestModule(const std::filesystem::path& layout, const std::wstring& culture)
{ return std::make_unique<KeyboardTestModule>(layout,culture,true,true,nullptr); }
std::unique_ptr<ITestModule> CreateKeyboardTestModuleForTesting(
    const std::filesystem::path& path, bool showWindow, std::function<void(HWND)> onCreated)
{
    const auto config = LoadModuleConfig(path);
    if (!config.success) return nullptr;
    auto layout=std::filesystem::path(config.config->GetValue(L"KeyboardTest",L"LayoutFile",L"ExternalTools\\Main.KBL"));
    if (layout.is_relative()) layout=path.parent_path()/layout;
    return std::make_unique<KeyboardTestModule>(layout,config.config->GetValue(L"language",L"Culture",L"zh-CN"),
        showWindow,false,std::move(onCreated));
}
}
