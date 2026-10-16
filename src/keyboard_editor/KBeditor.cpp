#include "KeyboardUi.h"
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <windowsx.h>
#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <optional>
#include <stdexcept>

namespace ftc::keyboard_editor
{
namespace
{
constexpr UINT jobMessage = WM_APP + 0x781, keyMessage = WM_APP + 0x782, scrollMessage = WM_APP + 0x783;
enum Id { New = 101, Open, Save, SaveAs, Add, Capture, Delete, Preview, Cancel, Apply, Culture = 201,
    Undo = 111, Redo, Details, NextRow,
    Name = 210, X, Y, Width, Height, Scan, Extended, LayoutWidth, LayoutHeight, Stick, BindingText };
struct EditSnapshot final { KeyboardLayout layout; int selected{-1}; };
struct Job final
{
    bool save{}, ok{};
    std::filesystem::path path;
    KeyboardLayout layout;
    std::wstring error;
};
struct Editor final
{
    HWND hwnd{}, pane{};
    HFONT font{};
    Canvas canvas;
    Language language{Language::Chinese};
    KeyboardLayout layout;
    std::filesystem::path path;
    std::map<int,HWND> controls;
    std::vector<std::pair<int,std::wstring>> labels;
    int selected{-1}, scroll{}, maxScroll{};
    UINT dpi{96};
    bool dirty{}, updating{}, busy{}, capturing{}, adding{}, preview{}, dragging{}, resize{}, closing{}, details{}, dragChanged{};
    bool scheduled{}, overflow{};
    int afterSave{};
    POINT dragStart{};
    LayoutKey dragKey;
    float dragScale{1};
    int nextX{10}, nextY{35};
    KeyGeometry geometry;
    HHOOK hook{};
    std::array<KeyEvent,1024> events{};
    std::array<bool,256> held{}, blocked{};
    std::size_t eventCount{};
    std::vector<Binding> captured, down;
    std::unique_ptr<KeyboardLogic> logic;
    std::wstring status;
    std::thread worker;
    std::mutex jobMutex;
    std::optional<Job> completedJob;
    std::vector<EditSnapshot> undo, redo;
    EditSnapshot shadow;
    std::wstring savedText;
};
thread_local Editor* active{};
const wchar_t* Tr(const Editor& e, const wchar_t* zh, const wchar_t* en, const wchar_t* vi)
{ return Text(e.language,zh,en,vi); }
void Error(const Editor& e, const std::wstring& message)
{ MessageBoxW(e.hwnd,message.c_str(),Tr(e,L"键盘布局编辑器",L"Keyboard layout editor",L"Trình sửa bố cục bàn phím"),MB_OK|MB_ICONERROR); }
bool Same(const Binding& a, const Binding& b)
{ return a.scan == b.scan && a.extended == b.extended && (a.scan != 0 || a.vk == b.vk); }
void StopHook(Editor& e)
{
    if (active == &e) active = nullptr;
    if (e.hook) { UnhookWindowsHookEx(e.hook); e.hook = nullptr; }
    e.eventCount = 0; e.overflow = false; e.scheduled = false;
}
LRESULT CALLBACK Hook(int code, WPARAM wparam, LPARAM lparam) noexcept
{
    Editor* e = active;
    if (code < 0 || !e)
        return CallNextHookEx(nullptr,code,wparam,lparam);
    if (code == HC_ACTION && (wparam == WM_KEYDOWN || wparam == WM_KEYUP || wparam == WM_SYSKEYDOWN || wparam == WM_SYSKEYUP))
    {
        const auto& value = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
        const bool blocked = value.vkCode < e->blocked.size() && e->blocked[value.vkCode];
        if (value.vkCode < e->held.size())
        {
            e->held[value.vkCode]=down && value.vkCode!=VK_PAUSE;
            if (!down || value.vkCode==VK_PAUSE) e->blocked[value.vkCode]=false;
        }
        if (GetForegroundWindow()!=e->hwnd || blocked) return CallNextHookEx(nullptr,code,wparam,lparam);
        if (e->eventCount < e->events.size())
            e->events[e->eventCount++] = {{value.scanCode,(value.flags & LLKHF_EXTENDED) != 0,value.vkCode},
                down,GetTickCount64()};
        else e->overflow = true;
        if (!e->scheduled) e->scheduled = PostMessageW(e->hwnd,keyMessage,0,0) != FALSE;
        return 1;
    }
    return CallNextHookEx(nullptr,code,wparam,lparam);
}
void UpdateTitle(Editor& e)
{
    e.dirty = SerializeLayout(e.layout)!=e.savedText;
    const auto title = std::wstring(Tr(e,L"键盘布局编辑器",L"Keyboard layout editor",L"Trình sửa bố cục bàn phím")) +
        L" · " + (e.path.empty() ? Tr(e,L"空白布局",L"Blank layout",L"Bố cục trống") : e.path.filename().wstring()) + (e.dirty ? L" *" : L"");
    SetWindowTextW(e.hwnd,title.c_str()); InvalidateRect(e.hwnd,nullptr,FALSE);
}
void Changed(Editor& e)
{
    if (SerializeLayout(e.layout)==SerializeLayout(e.shadow.layout)) return;
    e.logic.reset();
    if (!e.dragging || !e.dragChanged)
    {
        e.undo.push_back(e.shadow);
        if (e.undo.size()>60) e.undo.erase(e.undo.begin());
        e.redo.clear(); e.dragChanged=e.dragging;
    }
    e.shadow={e.layout,e.selected};
    UpdateTitle(e);
}
std::wstring Value(const Editor& e, int id)
{
    wchar_t text[1024]{}; GetWindowTextW(e.controls.at(id),text,1024); return text;
}
void Set(Editor& e, int id, const std::wstring& text) { SetWindowTextW(e.controls.at(id),text.c_str()); }
bool Integer(const Editor& e, int id, int& value, bool positive)
{
    const auto text = Value(e,id);
    if (text.empty() || text.size() > 5) return false;
    value = 0;
    for (const wchar_t c : text) { if (c < L'0' || c > L'9') return false; value = value * 10 + c - L'0'; }
    return value >= (positive ? 1 : 0) && value <= (id == Scan ? 65535 : 16384);
}
void Properties(Editor& e)
{
    e.updating = true;
    const bool selected = e.selected >= 0 && static_cast<std::size_t>(e.selected) < e.layout.keys.size();
    const auto key = selected ? e.layout.keys[static_cast<std::size_t>(e.selected)] : LayoutKey{};
    Set(e,Name,key.label); Set(e,X,std::to_wstring(key.x)); Set(e,Y,std::to_wstring(key.y));
    Set(e,Width,std::to_wstring(key.width)); Set(e,Height,std::to_wstring(key.height));
    Set(e,Scan,key.bindings.empty() ? L"" : std::to_wstring(key.bindings.front().scan));
    SendMessageW(e.controls.at(Extended),BM_SETCHECK,!key.bindings.empty() && key.bindings.front().extended ? BST_CHECKED : BST_UNCHECKED,0);
    Set(e,BindingText,key.bindings.empty() ? L"—" : BindingDescription(key.bindings));
    Set(e,LayoutWidth,std::to_wstring(e.layout.width)); Set(e,LayoutHeight,std::to_wstring(e.layout.height));
    SendMessageW(e.controls.at(Stick),BM_SETCHECK,e.layout.stickCheck ? BST_CHECKED : BST_UNCHECKED,0);
    e.updating = false;
}
void Enable(Editor& e)
{
    const bool editable = !e.busy && !e.capturing && !e.preview;
    for (const auto& [id,control] : e.controls)
    {
        bool enabled = editable;
        if (id == Culture) enabled = !e.busy && !e.capturing && !e.preview;
        if (id == Cancel) enabled = e.capturing || e.preview;
        if (id == NextRow) enabled = e.capturing && e.adding;
        if (id == Undo) enabled &= !e.undo.empty();
        if (id == Redo) enabled &= !e.redo.empty();
        if (id == Capture || id == Delete || (id >= Name && id <= Extended)) enabled &= e.selected >= 0;
        if (id == Scan || id == Extended)
            enabled &= e.selected >= 0 && e.layout.keys[static_cast<std::size_t>(e.selected)].bindings.size() == 1;
        if (id == Preview || id == Save || id == SaveAs) enabled &= !e.layout.keys.empty();
        EnableWindow(control,enabled);
    }
    ShowWindow(e.controls.at(Preview),e.capturing || e.preview ? SW_HIDE : SW_SHOW);
    ShowWindow(e.controls.at(Cancel),e.capturing || e.preview ? SW_SHOW : SW_HIDE);
    InvalidateRect(e.hwnd,nullptr,FALSE);
}
bool ApplyProperties(Editor& e)
{
    if (e.busy || e.capturing || e.preview) return false;
    int width{},height{};
    if (!Integer(e,LayoutWidth,width,true) || width < 32 || !Integer(e,LayoutHeight,height,true) || height < 33)
    { Error(e,Tr(e,L"布局宽高无效。",L"Invalid layout width / height.",L"Chiều rộng / cao bố cục không hợp lệ.")); return false; }
    KeyboardLayout updated = e.layout;
    updated.width = width; updated.height = height;
    updated.stickCheck = SendMessageW(e.controls.at(Stick),BM_GETCHECK,0,0) == BST_CHECKED;
    if (e.selected >= 0)
    {
        auto& key = updated.keys[static_cast<std::size_t>(e.selected)];
        const auto name = Value(e,Name);
        if (name.empty() || name.size() > 256 || name.find(L"][") != name.npos || name.find_first_of(L"\r\n") != name.npos ||
            !Integer(e,X,key.x,false) || !Integer(e,Y,key.y,false) || !Integer(e,Width,key.width,true) || !Integer(e,Height,key.height,true))
        { Error(e,Tr(e,L"请填写有效的按键名称、位置与尺寸。",L"Enter a valid key label, position and size.",L"Nhập tên, vị trí và kích thước phím hợp lệ.")); return false; }
        key.label = name;
        if (key.bindings.size() == 1)
        {
            int scan{}; if (!Integer(e,Scan,scan,false)) { Error(e,L"Invalid scan code"); return false; }
            const bool ext = SendMessageW(e.controls.at(Extended),BM_GETCHECK,0,0) == BST_CHECKED;
            auto& binding = key.bindings.front();
            if (binding.scan != static_cast<unsigned>(scan) || binding.extended != ext)
                binding = {static_cast<unsigned>(scan),ext,0};
            if (!binding.scan && !binding.vk) { Error(e,L"Scan code 0 requires an actual captured virtual key"); return false; }
        }
    }
    if (SerializeLayout(updated) != SerializeLayout(e.layout)) { e.layout = std::move(updated); Changed(e); }
    Properties(e); return true;
}
void Arrange(Editor& e)
{
    e.dpi = GetDpiForWindow(e.hwnd); if (!e.dpi) e.dpi = 96;
    if (e.font) DeleteObject(e.font);
    e.font = CreateFontW(-Px(13,e.dpi),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    RECT client{}; GetClientRect(e.hwnd,&client);
    const int width = MulDiv(client.right,96,static_cast<int>(e.dpi));
    const int rows = width < 1000 ? 2 : 1;
    for (int i = 0; i < 4; ++i) MoveWindow(e.controls.at(New+i),Px(24+i*116,e.dpi),Px(64,e.dpi),Px(108,e.dpi),Px(36,e.dpi),TRUE);
    MoveWindow(e.controls.at(Preview),Px(rows==1 ? 656 : 192,e.dpi),Px(rows==1 ? 64 : 108,e.dpi),Px(148,e.dpi),Px(36,e.dpi),TRUE);
    MoveWindow(e.controls.at(Undo),Px(rows==1 ? 488 : 24,e.dpi),Px(rows==1 ? 64 : 108,e.dpi),Px(76,e.dpi),Px(36,e.dpi),TRUE);
    MoveWindow(e.controls.at(Redo),Px(rows==1 ? 572 : 108,e.dpi),Px(rows==1 ? 64 : 108,e.dpi),Px(76,e.dpi),Px(36,e.dpi),TRUE);
    MoveWindow(e.controls.at(Culture),Px(width-186,e.dpi),Px(18,e.dpi),Px(162,e.dpi),Px(32,e.dpi),TRUE);
    MoveWindow(e.controls.at(Cancel),Px(rows==1 ? 656 : 192,e.dpi),Px(rows==1 ? 64 : 108,e.dpi),Px(148,e.dpi),Px(36,e.dpi),TRUE);
    ShowWindow(e.controls.at(Cancel),e.capturing || e.preview ? SW_SHOW : SW_HIDE);
    const int top = rows == 1 ? 158 : 202, bottom = MulDiv(client.bottom,96,static_cast<int>(e.dpi))-24;
    MoveWindow(e.pane,Px(24,e.dpi),Px(top,e.dpi),Px(272,e.dpi),std::max(1,Px(bottom-top,e.dpi)),TRUE);
    const int contentHeight=e.details ? 700 : 430;
    e.maxScroll = std::max(0,contentHeight-(bottom-top)); e.scroll = std::clamp(e.scroll,0,e.maxScroll);
    SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,contentHeight-1,static_cast<UINT>(std::max(1,bottom-top)),e.scroll,0};
    SetScrollInfo(e.pane,SB_VERT,&info,TRUE);
    const auto move = [&](int id,int x,int y,int w,int h) {
        MoveWindow(e.controls.at(id),Px(x,e.dpi),Px(y-e.scroll,e.dpi),Px(w,e.dpi),Px(h,e.dpi),TRUE);
    };
    move(Name,0,28,248,36); move(BindingText,0,94,248,54);
    move(Capture,0,160,248,36); move(Add,0,208,118,36); move(Delete,130,208,118,36);
    move(NextRow,0,252,248,36); move(Details,0,300,248,32);
    move(X,0,364,118,32); move(Y,130,364,118,32);
    move(Width,0,420,118,32); move(Height,130,420,118,32);
    move(Scan,0,476,118,32); move(Extended,130,476,118,32);
    move(LayoutWidth,0,532,118,32); move(LayoutHeight,130,532,118,32);
    move(Stick,0,582,248,36); move(Apply,0,e.details ? 644 : 372,248,36);
    const int ys[] = {4,68,340,396,452,508};
    for (std::size_t i = 0; i < e.labels.size(); ++i)
    {
        const int id = e.labels[i].first;
        move(id, i >= 2 && i%2 == 1 ? 130 : 0, i < 2 ? ys[i] : ys[2+(i-2)/2],i<2 ? 248 : 118,22);
    }
    for (int id=X;id<=Stick;++id) ShowWindow(e.controls.at(id),e.details ? SW_SHOW : SW_HIDE);
    for (std::size_t i=2;i<e.labels.size();++i) ShowWindow(e.controls.at(e.labels[i].first),e.details ? SW_SHOW : SW_HIDE);
    for (const auto& [id,control] : e.controls)
    {
        SendMessageW(control,WM_SETFONT,reinterpret_cast<WPARAM>(e.font),TRUE);
        if (id>=New && id<=NextRow && GetParent(control)==e.hwnd) PrepareButtonSurface(control,e.dpi);
    }
    InvalidateRect(e.hwnd,nullptr,FALSE); InvalidateRect(e.pane,nullptr,TRUE);
}
void Translate(Editor& e)
{
    const wchar_t* zh[] = {L"新建",L"打开",L"保存",L"另存为",L"添加按键",L"重新捕获",L"删除按键",L"预览测试",L"停止捕获 / 预览",L"应用属性"};
    const wchar_t* en[] = {L"New",L"Open",L"Save",L"Save as",L"Add key",L"Recapture",L"Delete key",L"Test preview",L"Stop capture / preview",L"Apply properties"};
    const wchar_t* vi[] = {L"Tạo mới",L"Mở",L"Lưu",L"Lưu thành",L"Thêm phím",L"Ghi lại",L"Xóa phím",L"Thử bố cục",L"Dừng ghi / thử",L"Áp dụng thuộc tính"};
    for (int i = 0; i < 10; ++i) SetWindowTextW(e.controls.at(New+i),Tr(e,zh[i],en[i],vi[i]));
    SetWindowTextW(e.controls.at(Add),Tr(e,L"连续添加",L"Add keys",L"Thêm phím"));
    SetWindowTextW(e.controls.at(Open),Tr(e,L"打开 KBL",L"Open KBL",L"Mở KBL"));
    SetWindowTextW(e.controls.at(Capture),Tr(e,L"按实体键重新绑定",L"Press key to rebind",L"Nhấn phím để gán lại"));
    SetWindowTextW(e.controls.at(Cancel),Tr(e,L"停止录入 / 预览",L"Stop capture / test",L"Dừng ghi / thử"));
    SetWindowTextW(e.controls.at(Undo),Tr(e,L"撤销",L"Undo",L"Hoàn tác"));
    SetWindowTextW(e.controls.at(Redo),Tr(e,L"重做",L"Redo",L"Làm lại"));
    SetWindowTextW(e.controls.at(NextRow),Tr(e,L"下一行",L"Next row",L"Hàng tiếp"));
    SetWindowTextW(e.controls.at(Details),Tr(e,e.details ? L"收起详细属性 ↑" : L"详细属性 ↓",
        e.details ? L"Hide details ↑" : L"Show details ↓",e.details ? L"Ẩn chi tiết ↑" : L"Xem chi tiết ↓"));
    const wchar_t* lzh[] = {L"按键名称",L"实际按键绑定",L"X",L"Y",L"按键宽度",L"按键高度",L"扫描码",L"扩展标志",L"布局宽度",L"布局高度"};
    const wchar_t* len[] = {L"Key label",L"Actual binding",L"X",L"Y",L"Key width",L"Key height",L"Scan code",L"Extended flag",L"Layout width",L"Layout height"};
    const wchar_t* lvi[] = {L"Tên phím",L"Sự kiện phím thực",L"X",L"Y",L"Rộng phím",L"Cao phím",L"Mã quét",L"Cờ mở rộng",L"Rộng bố cục",L"Cao bố cục"};
    for (std::size_t i = 0; i < e.labels.size(); ++i) SetWindowTextW(e.controls.at(e.labels[i].first),Tr(e,lzh[i],len[i],lvi[i]));
    SetWindowTextW(e.controls.at(Stick),Tr(e,L"20 ms 相邻按键检查",L"20 ms adjacent-key check",L"Kiểm tra phím kề 20 ms"));
    const auto title = std::wstring(Tr(e,L"键盘布局编辑器",L"Keyboard layout editor",L"Trình sửa bố cục bàn phím")) + L" · " +
        (e.path.empty() ? Tr(e,L"空白布局",L"Blank layout",L"Bố cục trống") : e.path.filename().wstring()) + (e.dirty ? L" *" : L"");
    SetWindowTextW(e.hwnd,title.c_str()); Arrange(e);
}
std::filesystem::path Choose(Editor& e, bool save)
{
    wchar_t file[32768]{};
    wchar_t module[32768]{}; GetModuleFileNameW(nullptr,module,32768);
    const auto initial = e.path.empty() ? (std::filesystem::path(module).parent_path()/L"Main.KBL").wstring() : e.path.wstring();
    wcsncpy_s(file,initial.c_str(),_TRUNCATE);
    OPENFILENAMEW info{sizeof(info)}; info.hwndOwner=e.hwnd; info.lpstrFilter=L"Keyboard layout (*.KBL)\0*.KBL\0\0";
    info.lpstrFile=file; info.nMaxFile=32768; info.lpstrDefExt=L"KBL";
    info.Flags=OFN_EXPLORER|OFN_NOCHANGEDIR|OFN_PATHMUSTEXIST|(save ? OFN_OVERWRITEPROMPT : OFN_FILEMUSTEXIST);
    if (save ? GetSaveFileNameW(&info) : GetOpenFileNameW(&info)) return file;
    const DWORD code = CommDlgExtendedError(); if (code) Error(e,L"File dialog: " + std::to_wstring(code));
    return {};
}
void StartJob(Editor& e, const std::filesystem::path& path, bool save)
{
    if (e.worker.joinable()) e.worker.join();
    const auto layout = e.layout;
    e.busy = true; e.status=Tr(e,save ? L"正在保存…" : L"正在打开…",save ? L"Saving…" : L"Opening…",save ? L"Đang lưu…" : L"Đang mở…");
    Enable(e);
    try { e.worker = std::thread([&e,path,save,layout] {
        Job job{save,false,path,layout,{}};
        try { job.ok = save ? WriteLayout(path,layout,job.error) : ReadLayout(path,job.layout,job.error); }
        catch (...) { job.error=L"KBL file operation failed"; }
        { std::lock_guard lock(e.jobMutex); e.completedJob=std::move(job); }
        PostMessageW(e.hwnd,jobMessage,0,0);
    }); }
    catch (...)
    {
        e.busy=false; e.closing=false; e.afterSave=0;
        e.status=Tr(e,L"无法启动文件操作。",L"Unable to start file operation.",L"Không thể bắt đầu thao tác tệp.");
        Enable(e); Error(e,e.status);
    }
}
void Command(Editor& e, int id, int notification);
void CompleteJob(Editor& e)
{
    std::optional<Job> job;
    { std::lock_guard lock(e.jobMutex); job=std::move(e.completedJob); e.completedJob.reset(); }
    if (!job) return;
    if (e.worker.joinable()) e.worker.join();
    e.busy=false;
    if (job->ok)
    {
        if (!job->save)
        {
            e.layout=std::move(job->layout); e.selected=e.layout.keys.empty() ? -1 : 0; e.scroll=0;
            e.logic.reset(); e.undo.clear(); e.redo.clear();
        }
        e.path=job->path; e.savedText=SerializeLayout(e.layout); e.shadow={e.layout,e.selected}; e.dirty=false;
        e.status=Tr(e,job->save ? L"布局已保存。" : L"布局已打开。",job->save ? L"Layout saved." : L"Layout opened.",
            job->save ? L"Đã lưu bố cục." : L"Đã mở bố cục.");
        Properties(e); Translate(e);
    }
    else { e.closing=false; e.afterSave=0; e.status=job->error; Error(e,job->error); }
    Enable(e);
    if (e.closing && job->ok) DestroyWindow(e.hwnd);
    else if (e.afterSave && job->ok) { const int next=std::exchange(e.afterSave,0); Command(e,next,0); }
}
bool LeaveDocument(Editor& e, bool closing, int afterSave = 0)
{
    if (!ApplyProperties(e)) return false;
    if (!e.dirty) return true;
    const int choice=MessageBoxW(e.hwnd,Tr(e,L"保存当前布局的修改吗？",L"Save changes to the current layout?",L"Lưu thay đổi bố cục hiện tại?"),
        L"KBeditor",MB_YESNOCANCEL|MB_ICONQUESTION);
    if (choice==IDCANCEL) return false;
    if (choice==IDNO) return true;
    if (e.layout.keys.empty())
    { Error(e,Tr(e,L"请先添加按键，或选择不保存。",L"Add keys first, or choose not to save.",L"Thêm phím trước, hoặc chọn không lưu.")); return false; }
    const auto path=e.path.empty() ? Choose(e,true) : e.path;
    if (path.empty()) return false;
    e.closing=closing; e.afterSave=afterSave; StartJob(e,path,true);
    return false;
}
bool StartHook(Editor& e)
{
    e.held.fill(false);
    for (unsigned vk=0;vk<e.blocked.size();++vk) e.blocked[vk]=(GetAsyncKeyState(static_cast<int>(vk))&0x8000)!=0;
    active=&e; e.hook=SetWindowsHookExW(WH_KEYBOARD_LL,Hook,GetModuleHandleW(nullptr),0);
    if (!e.hook) { active=nullptr; Error(e,L"SetWindowsHookEx: " + std::to_wstring(GetLastError())); return false; }
    return true;
}
void BeginCapture(Editor& e, bool adding)
{
    if (!ApplyProperties(e)) return;
    e.captured.clear(); e.down.clear(); e.adding=adding;
    e.nextX=10; e.nextY=35;
    if (adding && !e.layout.keys.empty())
    {
        const auto& key=e.layout.keys.back(); e.nextX=key.x+key.width+2; e.nextY=key.y;
    }
    if (!StartHook(e)) return;
    e.capturing=true;
    e.status=adding ? Tr(e,L"连续录入中：逐个按下并松开实体键；用鼠标换行或停止。",
        L"Adding keys: press and release each physical key. Click Next row or Stop.",
        L"Đang thêm phím: nhấn và nhả từng phím. Bấm Hàng tiếp hoặc Dừng.") :
        Tr(e,L"请按下并松开要绑定的实体键，Copilot 会记录整组事件。",
        L"Press and release the physical key to bind. Copilot records the full combination.",
        L"Nhấn và nhả phím cần gán. Copilot ghi toàn bộ tổ hợp.");
    Enable(e); Arrange(e); SetFocus(e.hwnd);
}
void Captured(Editor& e)
{
    if (!e.adding) { StopHook(e); e.capturing=false; }
    if (e.adding)
    {
        const auto existing=std::find_if(e.layout.keys.begin(),e.layout.keys.end(),[&](const LayoutKey& key) {
            return key.bindings.size()==e.captured.size() && std::all_of(key.bindings.begin(),key.bindings.end(),[&](const Binding& b) {
                return std::any_of(e.captured.begin(),e.captured.end(),[&](const Binding& c){return Same(b,c);});
            });
        });
        if (existing!=e.layout.keys.end())
        {
            e.selected=static_cast<int>(existing-e.layout.keys.begin()); e.shadow.selected=e.selected;
            e.status=Tr(e,L"该按键已经存在，已选中；可继续录入其他按键。",L"This key already exists and is selected. Continue with other keys.",
                L"Phím đã có và được chọn. Tiếp tục thêm phím khác.");
            e.captured.clear(); e.down.clear(); Properties(e); Enable(e); return;
        }
        if (e.layout.keys.size()>=512)
        {
            StopHook(e); e.capturing=false; e.captured.clear(); e.down.clear();
            e.status=Tr(e,L"已达到 512 个按键上限。",L"The 512-key limit has been reached.",L"Đã đạt giới hạn 512 phím.");
            Enable(e); return;
        }
        LayoutKey key; key.bindings=e.captured;
        key.label=IsCopilot(key.bindings) ? L"Copilot" : BindingName(key.bindings.back());
        if (key.bindings.size()>1 && !IsCopilot(key.bindings))
        { key.label.clear(); for (const auto& b:key.bindings) { if (!key.label.empty()) key.label+=L" + "; key.label+=BindingName(b); } }
        if (key.label.size()>256) key.label.resize(256);
        if (key.bindings.size()>1) key.width=IsCopilot(key.bindings) ? 88 : 110;
        if (e.nextX+key.width+22>16384)
        {
            e.status=Tr(e,L"当前行已达到布局最大宽度，请点击“下一行”继续。",
                L"This row reached the maximum layout width. Click Next row to continue.",
                L"Hàng đã đạt chiều rộng tối đa. Bấm Hàng tiếp để tiếp tục.");
            e.captured.clear(); e.down.clear(); Enable(e); return;
        }
        key.x=e.nextX; key.y=e.nextY;
        e.layout.width=std::max(e.layout.width,key.x+key.width+22);
        e.layout.height=std::max(e.layout.height,key.y+key.height+42);
        e.nextX=key.x+key.width+2;
        e.layout.keys.push_back(std::move(key)); e.selected=static_cast<int>(e.layout.keys.size()-1);
    }
    else if (e.selected>=0) e.layout.keys[static_cast<std::size_t>(e.selected)].bindings=e.captured;
    e.status=e.adding ? Tr(e,L"已添加，可继续按下一个实体键；用鼠标换行或停止录入。",
        L"Key added. Press the next physical key, or click Next row / Stop.",L"Đã thêm phím. Nhấn phím tiếp, hoặc bấm Hàng tiếp / Dừng.") :
        Tr(e,L"绑定已更新。拖动按键移动，拖动右下角调整大小，双击重新绑定。",
        L"Binding updated. Drag to move, drag the corner to resize, double-click to rebind.",
        L"Đã cập nhật. Kéo để di chuyển, kéo góc để đổi cỡ, nhấp đúp để gán lại.");
    e.captured.clear(); e.down.clear();
    Changed(e); Properties(e); Enable(e); Arrange(e);
}
void Consume(Editor& e)
{
    e.scheduled=false;
    if (e.overflow)
    {
        e.eventCount=0; e.overflow=false; e.captured.clear(); e.down.clear();
        if (e.logic) e.logic->Suspend();
        e.status=Tr(e,L"事件过多，请松开所有按键后重试。",L"Too many events. Release all keys and retry.",L"Quá nhiều sự kiện. Nhả mọi phím rồi thử lại.");
        InvalidateRect(e.hwnd,nullptr,FALSE); return;
    }
    const auto count=std::exchange(e.eventCount,0);
    for (std::size_t i=0;i<count;++i)
    {
        const auto event=e.events[i];
        if (e.preview && e.logic)
        {
            e.logic->Feed(event);
            if (e.logic->Complete())
            {
                StopHook(e); e.preview=false;
                e.status=Tr(e,L"预览测试全部通过。",L"Preview: all keys passed.",L"Thử bố cục: mọi phím đạt.");
                Enable(e); Arrange(e); break;
            }
        }
        else if (e.capturing)
        {
            auto found=std::find_if(e.down.begin(),e.down.end(),[&](const auto& b){return Same(b,event.binding);});
            if (event.down && found==e.down.end())
            {
                if (e.captured.size()>=8)
                {
                    e.down.clear(); e.captured.clear(); e.blocked=e.held;
                    e.status=Tr(e,L"一次最多记录 8 个事件，请松开所有按键后重试。",L"At most 8 events per key. Release all keys and retry.",
                        L"Tối đa 8 sự kiện mỗi phím. Nhả mọi phím rồi thử lại."); continue;
                }
                e.down.push_back(event.binding);
                if (std::none_of(e.captured.begin(),e.captured.end(),[&](const auto& b){return Same(b,event.binding);}))
                    e.captured.push_back(event.binding);
                if (event.binding.vk==VK_PAUSE && e.captured.size()==1) { e.down.clear(); Captured(e); if (!e.adding) break; }
            }
            else if (!event.down && found!=e.down.end())
            {
                e.down.erase(found);
                if (e.down.empty() && !e.captured.empty()) { Captured(e); if (!e.adding) break; }
            }
        }
    }
    InvalidateRect(e.hwnd,nullptr,FALSE);
}
void Paint(Editor& e)
{
    PAINTSTRUCT paint{}; const HDC dc=BeginPaint(e.hwnd,&paint);
    RECT r{}; GetClientRect(e.hwnd,&r);
    if (e.canvas.Begin(e.hwnd,e.dpi))
    {
        const float w=r.right*96.0F/e.dpi,h=r.bottom*96.0F/e.dpi;
        const float top=w<1000 ? 202.0F : 158.0F;
        e.canvas.Label(Tr(e,L"键盘布局编辑器",L"Keyboard layout editor",L"Trình sửa bố cục bàn phím"),{24,14,w-210,52},24,600);
        const auto instruction=e.status.empty() ? Tr(e,L"拖动按键移动 · 拖动右下角调整大小 · 双击按键重新绑定",
            L"Drag keys to move · Drag the corner to resize · Double-click to rebind",
            L"Kéo phím để di chuyển · Kéo góc để đổi cỡ · Nhấp đúp để gán lại") : e.status.c_str();
        e.canvas.Label(instruction,{24,top-48,w-24,top-8},13,400,Design.secondary,false,true);
        const D2D1_RECT_F area{320,top,w-24,h-54};
        if (!e.dragging) e.geometry=FitKeys(e.layout,area);
        const auto statuses=e.logic ? e.logic->Status() : std::vector<KeyStatus>{};
        DrawKeys(e.canvas,e.layout,statuses,e.geometry,e.selected);
        if (e.layout.keys.empty())
        {
            const float center=(area.top+area.bottom)/2;
            e.canvas.Label(Tr(e,L"建立你的键盘布局",L"Create your keyboard layout",L"Tạo bố cục bàn phím"),
                {area.left,center-65,area.right,center-15},22,600,Design.text,true);
            e.canvas.Label(Tr(e,L"点击左侧“连续添加”，逐个按下实体键；\n或点击“打开 KBL”修改已有布局。",
                L"Click Add keys and press each physical key,\nor use Open KBL to edit an existing layout.",
                L"Bấm Thêm phím và nhấn từng phím thực,\nhoặc Mở KBL để sửa bố cục đã có."),
                {area.left,center-8,area.right,center+65},14,400,Design.secondary,true,true);
        }
        const auto count=std::to_wstring(e.layout.keys.size())+Tr(e,L" 个按键",L" keys",L" phím")+
            (e.logic ? L" · PASS " + std::to_wstring(e.logic->Passed()) : L"");
        e.canvas.Label(count,{320,h-46,w-24,h-20},13,400,Design.secondary);
        e.canvas.End(e.hwnd);
    }
    else { FillRect(dc,&r,GetSysColorBrush(COLOR_WINDOW)); DrawTextW(dc,L"KBeditor",-1,&r,DT_CENTER|DT_SINGLELINE); }
    EndPaint(e.hwnd,&paint);
}
LRESULT CALLBACK PaneProcedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR,DWORD_PTR data)
{
    auto& e=*reinterpret_cast<Editor*>(data);
    if (message==WM_VSCROLL || message==WM_MOUSEWHEEL)
    {
        int position=e.scroll;
        if (message==WM_MOUSEWHEEL) position-=GET_WHEEL_DELTA_WPARAM(wparam)/WHEEL_DELTA*36;
        else switch (LOWORD(wparam))
        {
        case SB_LINEUP: position-=20; break; case SB_LINEDOWN: position+=20; break;
        case SB_PAGEUP: position-=120; break; case SB_PAGEDOWN: position+=120; break;
        case SB_THUMBTRACK: { SCROLLINFO info{sizeof(info),SIF_TRACKPOS}; GetScrollInfo(hwnd,SB_VERT,&info); position=info.nTrackPos; break; }
        default: break;
        }
        e.scroll=std::clamp(position,0,e.maxScroll); SendMessageW(e.hwnd,scrollMessage,0,0); return 0;
    }
    if (message==WM_CTLCOLORSTATIC || message==WM_CTLCOLOREDIT || message==WM_CTLCOLORBTN)
    {
        const HDC dc=reinterpret_cast<HDC>(wparam); SetTextColor(dc,RGB(36,36,36));
        const COLORREF color = message==WM_CTLCOLOREDIT ? RGB(255,255,255) : RGB(243,243,243);
        SetBkColor(dc,color); SetDCBrushColor(dc,color);
        return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
    }
    if (message==WM_ERASEBKGND)
    { SetDCBrushColor(reinterpret_cast<HDC>(wparam),RGB(243,243,243)); RECT r{}; GetClientRect(hwnd,&r); FillRect(reinterpret_cast<HDC>(wparam),&r,static_cast<HBRUSH>(GetStockObject(DC_BRUSH))); return 1; }
    if (message==WM_PAINT)
    {
        PAINTSTRUCT paint{}; const auto dc=BeginPaint(hwnd,&paint); RECT r{}; GetClientRect(hwnd,&r);
        SetDCBrushColor(dc,RGB(243,243,243)); FillRect(dc,&r,static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        EndPaint(hwnd,&paint); return 0;
    }
    if (message==WM_COMMAND || message==WM_DRAWITEM) return SendMessageW(e.hwnd,message,wparam,lparam);
    return DefSubclassProc(hwnd,message,wparam,lparam);
}
void CreateControls(Editor& e)
{
    for (int id=New;id<=NextRow;++id)
    {
        const HWND parent=id==Apply || id==Add || id==Capture || id==Delete || id==Details || id==NextRow ? e.pane : e.hwnd;
        e.controls[id]=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,parent,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
    }
    e.controls[Culture]=CreateWindowExW(WS_EX_LAYERED,WC_COMBOBOXW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,0,0,1,1,e.hwnd,
        reinterpret_cast<HMENU>(Culture),GetModuleHandleW(nullptr),nullptr);
    for (const wchar_t* text:{L"中文",L"English",L"Tiếng Việt"}) SendMessageW(e.controls[Culture],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(text));
    SendMessageW(e.controls[Culture],CB_SETCURSEL,static_cast<WPARAM>(e.language),0);
    for (int id=Name;id<=LayoutHeight;++id)
    {
        if (id==Extended)
            e.controls[id]=CreateWindowW(L"BUTTON",L"E0",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,0,0,1,1,e.pane,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
        else
        {
            e.controls[id]=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL|
                (id==Name ? 0 : ES_NUMBER),0,0,1,1,e.pane,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),GetModuleHandleW(nullptr),nullptr);
            SendMessageW(e.controls[id],EM_SETLIMITTEXT,id==Name ? 256 : 5,0);
        }
    }
    e.controls[BindingText]=CreateWindowW(L"EDIT",L"",WS_CHILD|WS_VISIBLE|ES_MULTILINE|ES_READONLY|WS_VSCROLL,0,0,1,1,e.pane,
        reinterpret_cast<HMENU>(BindingText),GetModuleHandleW(nullptr),nullptr);
    e.controls[Stick]=CreateWindowW(L"BUTTON",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,0,0,1,1,e.pane,
        reinterpret_cast<HMENU>(Stick),GetModuleHandleW(nullptr),nullptr);
    for (int i=0;i<10;++i)
    {
        const int id=400+i;
        e.controls[id]=CreateWindowW(L"STATIC",L"",WS_CHILD|WS_VISIBLE,0,0,1,1,e.pane,reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
            GetModuleHandleW(nullptr),nullptr);
        e.labels.emplace_back(id,L"");
    }
    for (const auto& [id,control]:e.controls) { static_cast<void>(id); if (!control) throw std::runtime_error("CreateWindow(control)"); }
    SetLayeredWindowAttributes(e.pane,0,255,LWA_ALPHA);
    SetLayeredWindowAttributes(e.controls[Culture],0,255,LWA_ALPHA);
}
void Command(Editor& e, int id, int notification)
{
    if (e.busy) return;
    if (id==Cancel)
    {
        const bool preview=e.preview;
        StopHook(e); e.capturing=e.preview=false; e.captured.clear(); e.down.clear(); e.logic.reset();
        e.status=preview ? Tr(e,L"已退出预览。",L"Test preview stopped.",L"Đã dừng thử bố cục.") :
            Tr(e,L"已停止录入，已添加的按键会保留。",L"Capture stopped. Added keys are kept.",L"Đã dừng ghi. Các phím đã thêm được giữ lại.");
        Enable(e); Arrange(e); return;
    }
    if (id==NextRow && e.capturing && e.adding)
    {
        e.nextX=10;
        int bottom=e.nextY+50;
        for (const auto& key:e.layout.keys) if (key.y>=e.nextY && key.y<=bottom) bottom=std::max(bottom,key.y+key.height);
        e.nextY=std::min(16292,bottom+2);
        e.status=Tr(e,L"下一行已就绪，请继续按下实体键。",L"Next row ready. Continue pressing physical keys.",L"Hàng tiếp đã sẵn sàng. Tiếp tục nhấn phím thực.");
        InvalidateRect(e.hwnd,nullptr,FALSE); SetFocus(e.hwnd); return;
    }
    if (e.capturing || e.preview) return;
    if (id==Culture && notification==CBN_SELCHANGE)
    {
        e.language=static_cast<Language>(SendMessageW(e.controls[Culture],CB_GETCURSEL,0,0)); e.status.clear(); Translate(e); return;
    }
    if (id==Apply) { if (ApplyProperties(e)) Enable(e); return; }
    if (id==Details)
    { e.details=!e.details; e.scroll=0; Translate(e); return; }
    if (id==Undo || id==Redo)
    {
        if (!ApplyProperties(e)) return;
        auto& source=id==Undo ? e.undo : e.redo; auto& destination=id==Undo ? e.redo : e.undo;
        if (source.empty()) return;
        destination.push_back({e.layout,e.selected}); const auto snapshot=std::move(source.back()); source.pop_back();
        e.layout=snapshot.layout; e.selected=snapshot.selected; e.shadow={e.layout,e.selected}; e.logic.reset();
        e.status=Tr(e,id==Undo ? L"已撤销。" : L"已重做。",id==Undo ? L"Undone." : L"Redone.",id==Undo ? L"Đã hoàn tác." : L"Đã làm lại.");
        UpdateTitle(e); Properties(e); Enable(e); return;
    }
    if (id==New || id==Open)
    {
        if (!LeaveDocument(e,false,id)) return;
        if (id==Open) { const auto path=Choose(e,false); if (!path.empty()) StartJob(e,path,false); }
        else
        {
            e.layout={}; e.layout.width=986; e.layout.height=300; e.path.clear(); e.selected=-1; e.logic.reset();
            e.savedText=SerializeLayout(e.layout); e.shadow={e.layout,e.selected}; e.undo.clear(); e.redo.clear(); e.scroll=0;
            e.dirty=false; e.status.clear(); Properties(e); Translate(e); Enable(e);
        }
    }
    else if (id==Save || id==SaveAs)
    {
        if (!ApplyProperties(e) || e.layout.keys.empty()) return;
        const auto path=id==SaveAs || e.path.empty() ? Choose(e,true) : e.path;
        if (!path.empty()) StartJob(e,path,true);
    }
    else if (id==Add || id==Capture) BeginCapture(e,id==Add);
    else if (id==Delete && e.selected>=0)
    {
        e.layout.keys.erase(e.layout.keys.begin()+e.selected);
        e.selected=e.layout.keys.empty() ? -1 : std::min(e.selected,static_cast<int>(e.layout.keys.size())-1);
        e.logic.reset(); Changed(e); Properties(e); Enable(e);
    }
    else if (id==Preview && ApplyProperties(e) && !e.layout.keys.empty())
    {
        e.logic=std::make_unique<KeyboardLogic>(e.layout);
        if (!StartHook(e)) { e.logic.reset(); return; }
        e.preview=true; e.status=Tr(e,L"预览测试：逐键按下并松开。",L"Preview: press and release each key.",L"Thử bố cục: nhấn và nhả từng phím.");
        Enable(e); Arrange(e); SetFocus(e.hwnd);
    }
}
LRESULT CALLBACK Procedure(HWND hwnd,UINT message,WPARAM wparam,LPARAM lparam) noexcept
{
    auto* e=reinterpret_cast<Editor*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
    if (message==WM_NCCREATE)
    { e=static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); e->hwnd=hwnd; SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(e)); }
    if (!e) return DefWindowProcW(hwnd,message,wparam,lparam);
    try
    {
        switch (message)
        {
        case WM_CREATE:
            e->canvas.Attach(hwnd);
            e->pane=CreateWindowExW(WS_EX_LAYERED,L"STATIC",L"",WS_CHILD|WS_VISIBLE|WS_CLIPCHILDREN|WS_VSCROLL,0,0,1,1,hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
            if (!e->pane || !SetWindowSubclass(e->pane,PaneProcedure,1,reinterpret_cast<DWORD_PTR>(e))) return -1;
            CreateControls(*e); Properties(*e); Translate(*e); Enable(*e);
            if (!SetTimer(hwnd,1,50,nullptr)) return -1;
            return 0;
        case WM_SIZE: case scrollMessage: Arrange(*e); return 0;
        case WM_GETMINMAXINFO:
        {
            MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor);
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize={std::min<long>(Px(920,e->dpi),monitor.rcWork.right-monitor.rcWork.left),
                std::min<long>(Px(520,e->dpi),monitor.rcWork.bottom-monitor.rcWork.top)}; return 0;
        }
        case WM_DPICHANGED:
        {
            const auto& r=*reinterpret_cast<RECT*>(lparam); SetWindowPos(hwnd,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
            Arrange(*e); return 0;
        }
        case WM_ACTIVATE:
            if (LOWORD(wparam)==WA_INACTIVE)
            {
                e->eventCount=0; e->captured.clear(); e->down.clear();
                if (e->logic) e->logic->Suspend();
            }
            else for (unsigned vk=0;vk<e->blocked.size();++vk) e->blocked[vk]=e->held[vk] || (GetAsyncKeyState(static_cast<int>(vk))&0x8000)!=0;
            e->canvas.Configure(); return 0;
        case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT: case WM_CTLCOLORLISTBOX:
        {
            const HDC dc=reinterpret_cast<HDC>(wparam); SetTextColor(dc,RGB(36,36,36)); SetBkColor(dc,RGB(243,243,243));
            SetDCBrushColor(dc,RGB(243,243,243)); return reinterpret_cast<LRESULT>(GetStockObject(DC_BRUSH));
        }
        case WM_COMMAND: Command(*e,LOWORD(wparam),HIWORD(wparam)); return 0;
        case WM_DRAWITEM: DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam),e->dpi,e->font,
            reinterpret_cast<DRAWITEMSTRUCT*>(lparam)->CtlID==Save); return TRUE;
        case WM_PAINT: Paint(*e); return 0;
        case WM_ERASEBKGND: return 1;
        case jobMessage: CompleteJob(*e); return 0;
        case keyMessage: Consume(*e); return 0;
        case WM_TIMER:
            if (wparam!=1) break;
            if (e->busy) { CompleteJob(*e); return 0; }
            if (e->eventCount || e->overflow) Consume(*e);
            return 0;
        case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        {
            if (e->busy || e->capturing || e->preview) return 0;
            const float x=GET_X_LPARAM(lparam)*96.0F/e->dpi,y=GET_Y_LPARAM(lparam)*96.0F/e->dpi;
            for (std::size_t i=e->layout.keys.size();i>0;--i)
            {
                const auto rect=e->geometry.Rect(e->layout.keys[i-1]);
                if (x>=rect.left && x<=rect.right && y>=rect.top && y<=rect.bottom)
                {
                    if (!ApplyProperties(*e)) return 0;
                    e->selected=static_cast<int>(i-1); e->shadow.selected=e->selected; e->logic.reset(); Properties(*e); Enable(*e); SetFocus(hwnd);
                    if (message==WM_LBUTTONDBLCLK) { BeginCapture(*e,false); return 0; }
                    e->dragging=true; e->dragStart={GET_X_LPARAM(lparam),GET_Y_LPARAM(lparam)};
                    e->dragChanged=false; e->dragScale=e->geometry.scale;
                    e->dragKey=e->layout.keys[i-1]; e->resize=x>=rect.right-12 && y>=rect.bottom-12;
                    SetCapture(hwnd); break;
                }
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (e->dragging && e->selected>=0)
            {
                auto& key=e->layout.keys[static_cast<std::size_t>(e->selected)];
                const int dx=static_cast<int>((GET_X_LPARAM(lparam)-e->dragStart.x)*96.0F/e->dpi/e->dragScale);
                const int dy=static_cast<int>((GET_Y_LPARAM(lparam)-e->dragStart.y)*96.0F/e->dpi/e->dragScale);
                if (e->resize) { key.width=std::clamp(e->dragKey.width+dx,8,16384); key.height=std::clamp(e->dragKey.height+dy,8,16384); }
                else { key.x=std::clamp(e->dragKey.x+dx,0,std::max(0,e->layout.width-key.width-12)); key.y=std::clamp(e->dragKey.y+dy,0,std::max(0,e->layout.height-key.height-32)); }
                Changed(*e); Properties(*e);
            }
            return 0;
        case WM_LBUTTONUP: if (e->dragging) { e->dragging=false; ReleaseCapture(); InvalidateRect(hwnd,nullptr,FALSE); } return 0;
        case WM_CAPTURECHANGED: e->dragging=false; InvalidateRect(hwnd,nullptr,FALSE); return 0;
        case WM_SETCURSOR:
            if (LOWORD(lparam)==HTCLIENT && e->selected>=0 && !e->capturing && !e->preview)
            {
                POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd,&point);
                const float x=point.x*96.0F/e->dpi,y=point.y*96.0F/e->dpi;
                const auto rect=e->geometry.Rect(e->layout.keys[static_cast<std::size_t>(e->selected)]);
                if ((e->resize && e->dragging) || (x>=rect.right-12 && x<=rect.right && y>=rect.bottom-12 && y<=rect.bottom))
                { SetCursor(LoadCursorW(nullptr,IDC_SIZENWSE)); return TRUE; }
            }
            break;
        case WM_KEYDOWN:
            if (!e->busy && !e->capturing && !e->preview && e->selected>=0)
            {
                if (wparam==VK_DELETE) { Command(*e,Delete,0); return 0; }
                if (wparam>=VK_LEFT && wparam<=VK_DOWN)
                {
                    auto& key=e->layout.keys[static_cast<std::size_t>(e->selected)];
                    int& horizontal=(GetKeyState(VK_CONTROL)&0x8000) ? key.width : key.x;
                    int& vertical=(GetKeyState(VK_CONTROL)&0x8000) ? key.height : key.y;
                    if (wparam==VK_LEFT || wparam==VK_RIGHT) horizontal=std::clamp(horizontal+(wparam==VK_LEFT ? -2 : 2),
                        (GetKeyState(VK_CONTROL)&0x8000) ? 1 : 0,16384);
                    else vertical=std::clamp(vertical+(wparam==VK_UP ? -2 : 2),(GetKeyState(VK_CONTROL)&0x8000) ? 1 : 0,16384);
                    Changed(*e); Properties(*e); return 0;
                }
            }
            break;
        case WM_CLOSE:
            if (e->busy) return 0;
            if (e->capturing || e->preview) Command(*e,Cancel,0);
            if (LeaveDocument(*e,true)) DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY: StopHook(*e); KillTimer(hwnd,1); PostQuitMessage(0); return 0;
        }
    }
    catch (const std::exception&) { Error(*e,L"Editor operation failed"); }
    catch (...) { Error(*e,L"Editor operation failed"); }
    return DefWindowProcW(hwnd,message,wparam,lparam);
}
}
int Run(HINSTANCE instance)
{
    Editor editor;
    int argc{}; wchar_t** argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    std::filesystem::path initial;
    for (int i=1;argv && i<argc;++i)
    {
        if (_wcsicmp(argv[i],L"--language")==0 && i+1<argc)
        { const auto language=argv[++i]; editor.language=_wcsicmp(language,L"en-US")==0 ? Language::English :
            _wcsicmp(language,L"vi-VN")==0 ? Language::Vietnamese : Language::Chinese; }
        else initial=argv[i];
    }
    if (argv) LocalFree(argv);
    editor.savedText=SerializeLayout(editor.layout); editor.shadow={editor.layout,editor.selected};
    INITCOMMONCONTROLSEX controls{sizeof(controls),ICC_STANDARD_CLASSES}; InitCommonControlsEx(&controls);
    WNDCLASSW wc{}; wc.style=CS_DBLCLKS; wc.hInstance=instance; wc.lpfnWndProc=Procedure; wc.lpszClassName=L"FTC.KBeditor"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW);
    wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(101));
    if (!RegisterClassW(&wc)) return 1;
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromPoint({0,0},MONITOR_DEFAULTTOPRIMARY),&monitor);
    editor.dpi=GetDpiForSystem();
    const auto width=std::min<long>(Px(1220,editor.dpi),monitor.rcWork.right-monitor.rcWork.left);
    const auto height=std::min<long>(Px(760,editor.dpi),monitor.rcWork.bottom-monitor.rcWork.top);
    const HWND hwnd=CreateWindowExW(0,wc.lpszClassName,L"KBeditor",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        monitor.rcWork.left+(monitor.rcWork.right-monitor.rcWork.left-width)/2,
        monitor.rcWork.top+(monitor.rcWork.bottom-monitor.rcWork.top-height)/2,width,height,nullptr,nullptr,instance,&editor);
    if (!hwnd) return 1;
    ShowPrepared(hwnd);
    if (!initial.empty()) StartJob(editor,initial,false);
    ACCEL accelerators[]={{FVIRTKEY|FCONTROL,'S',Save},{FVIRTKEY|FCONTROL,'O',Open},{FVIRTKEY|FCONTROL,'N',New},
        {FVIRTKEY|FCONTROL,'Z',Undo},{FVIRTKEY|FCONTROL,'Y',Redo}};
    const HACCEL accelerator=CreateAcceleratorTableW(accelerators,static_cast<int>(std::size(accelerators)));
    MSG message{};
    while (GetMessageW(&message,nullptr,0,0)>0)
    {
        wchar_t focusedClass[32]{}; GetClassNameW(GetFocus(),focusedClass,32);
        const bool textUndo=message.message==WM_KEYDOWN && (message.wParam=='Z' || message.wParam=='Y') &&
            (GetKeyState(VK_CONTROL)&0x8000) && _wcsicmp(focusedClass,L"EDIT")==0;
        if (!textUndo && TranslateAcceleratorW(hwnd,accelerator,&message)) continue;
        if (GetFocus()!=hwnd && IsDialogMessageW(hwnd,&message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    if (accelerator) DestroyAcceleratorTable(accelerator);
    if (editor.worker.joinable()) editor.worker.join();
    if (editor.font) DeleteObject(editor.font);
    return 0;
}
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,PWSTR,int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try { return ftc::keyboard_editor::Run(instance); }
    catch (...) { MessageBoxW(nullptr,L"KBeditor startup failed",L"KBeditor",MB_OK|MB_ICONERROR); return 1; }
}
