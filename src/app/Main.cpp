#include "Application.h"
#include <shellapi.h>
#include <stdexcept>

int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try
    {
        int argc{};
        struct Arguments final { wchar_t** value{}; ~Arguments() { if (value) LocalFree(value); } };
        Arguments arguments{CommandLineToArgvW(GetCommandLineW(),&argc)};
        if (!arguments.value) throw std::runtime_error("CommandLineToArgvW failed");
        if (argc>2)
        {
            MessageBoxW(nullptr,L"用法 / Usage: KeyboardTest.exe [KBL path]\r\n路径包含空格时请使用双引号 / Quote paths containing spaces.",
                L"KeyboardTest",MB_OK|MB_ICONERROR);
            return 1;
        }
        const auto directory=std::filesystem::current_path();
        const auto layout=keyboard_app::ResolveLayoutPath(directory,argc>1 ? arguments.value[1] : L"Main.KBL");
        return keyboard_app::RunApplication(directory,layout);
    }
    catch (...)
    {
        MessageBoxW(nullptr,L"无法启动键盘测试 / Cannot start KeyboardTest.",L"KeyboardTest",MB_OK|MB_ICONERROR);
        return 3;
    }
}
