#include "Application.h"
#include "modules/ModuleConfig.h"
#include "modules/keyboard_test/KeyboardTestModule.h"
#include <atomic>
#include <mutex>
#include <limits>
#include <stdexcept>

namespace keyboard_app
{
using namespace ftc::modules;
namespace
{
struct Handle final
{
    HANDLE value{};
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
std::wstring Timestamp()
{
    SYSTEMTIME now{}; GetLocalTime(&now);
    wchar_t value[48]{};
    swprintf_s(value,L"%04u-%02u-%02u %02u:%02u:%02u.%03u",now.wYear,now.wMonth,now.wDay,
        now.wHour,now.wMinute,now.wSecond,now.wMilliseconds);
    return value;
}
const wchar_t* ResultName(TestResult result)
{ return result == TestResult::Pass ? L"PASS" : result == TestResult::Fail ? L"FAIL" : L"CANCEL"; }
DWORD AppendLog(const std::filesystem::path& path, const std::wstring& text)
{
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(),error);
    if (error) return static_cast<DWORD>(error.value());
    if (text.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) return ERROR_FILE_TOO_LARGE;
    const auto size = static_cast<int>(text.size());
    const int length = WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),size,nullptr,0,nullptr,nullptr);
    if (!length) return GetLastError();
    std::string bytes(static_cast<std::size_t>(length),'\0');
    if (!WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,text.data(),size,bytes.data(),length,nullptr,nullptr))
        return GetLastError();
    Handle file{CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,
        OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr)};
    if (file.value == INVALID_HANDLE_VALUE) return GetLastError();
    OVERLAPPED overlap{};
    if (!LockFileEx(file.value,LOCKFILE_EXCLUSIVE_LOCK,0,MAXDWORD,MAXDWORD,&overlap)) return GetLastError();
    DWORD status = ERROR_SUCCESS;
    LARGE_INTEGER end{};
    if (!SetFilePointerEx(file.value,end,nullptr,FILE_END)) status=GetLastError();
    DWORD written{};
    if (!status && !WriteFile(file.value,bytes.data(),static_cast<DWORD>(bytes.size()),&written,nullptr))
        status=GetLastError();
    if (!status && written != bytes.size()) status=ERROR_WRITE_FAULT;
    if (!status && !FlushFileBuffers(file.value)) status=GetLastError();
    UnlockFileEx(file.value,0,MAXDWORD,MAXDWORD,&overlap);
    return status;
}
}
std::filesystem::path ResolveLayoutPath(const std::filesystem::path& workingDirectory,
    const std::filesystem::path& argument)
{
    if (argument.empty()) throw std::invalid_argument("KBL path is empty");
    return (argument.is_absolute() ? argument : workingDirectory/argument).lexically_normal();
}
int RunApplication(const std::filesystem::path& directory, const std::filesystem::path& layoutPath,
    ModuleFactory factory, const bool reportErrors)
{
    Handle finished{CreateEventW(nullptr,TRUE,FALSE,nullptr)};
    if (!finished.value) throw std::runtime_error("CreateEventW failed");
    const auto logPath=directory/L"KeyBoard.log";
    const auto started=GetTickCount64();
    const auto startedText=Timestamp();
    wchar_t machine[MAX_COMPUTERNAME_LENGTH+1]{};
    DWORD machineSize=static_cast<DWORD>(std::size(machine));
    GetComputerNameW(machine,&machineSize);
    std::wstring log=L"\r\n========== KeyboardTest " KEYBOARD_VERSION L" ==========\r\n";
    log+=L"Start: "+startedText+L"\r\nComputer: "+machine+L"\r\nProcess: "+std::to_wstring(GetCurrentProcessId());
    log+=L"\r\nWorkingDirectory: "+directory.wstring()+L"\r\nLayout: "+layoutPath.wstring()+L"\r\n";
    std::mutex mutex;
    std::atomic_bool completing{};
    TestResult result=TestResult::Fail;
    DWORD logError{};
    std::wstring startupError;
    ModuleCallbacks callbacks;
    callbacks.onLog=[&](LogMessage message) {
        std::lock_guard lock(mutex);
        const auto level=message.level==LogLevel::Error ? L"ERROR" : message.level==LogLevel::Warning ? L"WARN" : L"INFO";
        log+=Timestamp()+L" ["+level+L"] "+message.message+L"\r\n";
    };
    callbacks.onCompletion=[&](Completion completion) noexcept {
        if (completing.exchange(true)) return;
        result=completion.result;
        try
        {
            std::lock_guard lock(mutex);
            log+=L"End: "+Timestamp()+L"\r\nDurationMs: "+std::to_wstring(GetTickCount64()-started);
            log+=L"\r\nResult: "+std::wstring(ResultName(result))+L"\r\n"+completion.message;
            log+=L"\r\n================================================\r\n";
            logError=AppendLog(logPath,log);
        }
        catch (...) { logError=ERROR_UNHANDLED_EXCEPTION; }
        SetEvent(finished.value); // Process shutdown is only allowed after the append/flush finishes.
    };
    std::unique_ptr<ITestModule> module;
    try
    {
        std::wstring culture=L"zh-CN";
        if (std::filesystem::exists(directory/L"config.ini"))
        {
            const auto config=LoadModuleConfig(directory/L"config.ini");
            if (!config.success) startupError=config.error;
            else culture=config.config->GetValue(L"language",L"Culture",L"zh-CN");
        }
        if (startupError.empty())
        {
            module=factory ? factory() : keyboard_test::CreateKeyboardTestModule(layoutPath,culture);
            if (!module) startupError=L"Cannot create keyboard test module";
            else
            {
                const auto outcome=module->Start(nullptr,callbacks);
                if (!outcome.IsStarted()) startupError=outcome.message;
            }
        }
    }
    catch (...) { startupError=L"KeyboardTest startup failed unexpectedly"; }
    if (!startupError.empty()) callbacks.onCompletion({TestResult::Fail,startupError});
    while (WaitForSingleObject(finished.value,0)!=WAIT_OBJECT_0)
    {
        const auto wait=MsgWaitForMultipleObjectsEx(1,&finished.value,INFINITE,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        if (wait==WAIT_OBJECT_0) break;
        if (wait==WAIT_FAILED)
        {
            if (module) module->RequestStop();
            callbacks.onCompletion({TestResult::Fail,L"MsgWaitForMultipleObjectsEx failed: "+std::to_wstring(GetLastError())});
            break;
        }
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE))
        {
            if (message.message==WM_QUIT) { if (module) module->RequestStop(); continue; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    module.reset(); // Join the module's report thread before releasing callbacks/event/log storage.
    if (reportErrors && logError)
    {
        const auto text=L"测试结果 / Result: "+std::wstring(ResultName(result))+
            L"\r\n日志保存失败 / Could not save log:\r\n"+logPath.wstring()+L"\r\nWindows error: "+std::to_wstring(logError);
        MessageBoxW(nullptr,text.c_str(),L"KeyboardTest",MB_OK|MB_ICONERROR);
    }
    else if (reportErrors && !startupError.empty())
        MessageBoxW(nullptr,startupError.c_str(),L"KeyboardTest",MB_OK|MB_ICONERROR);
    if (logError) return 3;
    return result==TestResult::Pass ? 0 : result==TestResult::Fail ? 1 : 2;
}
}
