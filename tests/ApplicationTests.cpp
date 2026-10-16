#include "app/Application.h"
#include "modules/keyboard_test/KeyboardTestModule.h"
#include <fstream>
#include <iostream>
#include <stdexcept>

using namespace ftc::modules;
using namespace ftc::modules::keyboard_test;
namespace
{
bool closeTest{};
void Check(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
void CALLBACK CompleteFixture(HWND hwnd,UINT,UINT_PTR timer,DWORD)
{
    if (!IsWindowEnabled(GetDlgItem(hwnd,101))) return;
    KillTimer(hwnd,timer);
    if (closeTest) { SendMessageW(hwnd,WM_CLOSE,0,0); return; }
    for (const unsigned scan:{30U,48U})
    {
        SendMessageW(hwnd,KeyboardFixtureEvent,1,scan);
        SendMessageW(hwnd,KeyboardFixtureEvent,0,scan);
    }
}
std::string Read(const std::filesystem::path& path)
{
    std::ifstream file(path,std::ios::binary);
    return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
}
int wmain()
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto root=std::filesystem::temp_directory_path()/(L"KeyboardApplicationTests-"+std::to_wstring(GetCurrentProcessId()));
    try
    {
        std::filesystem::create_directories(root);
        const auto layout=root/L"键盘 测试.KBL";
        Check(keyboard_app::ResolveLayoutPath(root)==root/L"Main.KBL","default layout must use working directory");
        Check(keyboard_app::ResolveLayoutPath(root,L"KeyBoard.KBL")==root/L"KeyBoard.KBL","relative layout path");
        Check(keyboard_app::ResolveLayoutPath(root,L"sub/../键盘 测试.KBL")==layout,"relative Unicode/spaces path");
        Check(keyboard_app::ResolveLayoutPath(root,layout)==layout,"absolute layout path");
        { std::ofstream file(layout); file<<"2][300][200][0\nA][10][35][50][50][30][0\nB][62][35][50][50][48][0\n"; }
        { std::ofstream file(root/L"config.ini"); file<<"[language]\nCulture=en-US\n[KeyboardTest]\nLayoutFile=";
          // Keep fixture INI ASCII while exercising Unicode in the real file path.
          file<<"Fixture.KBL\n"; }
        std::filesystem::copy_file(layout,root/L"Fixture.KBL");
        const auto fixture=[&] { return CreateKeyboardTestModuleForTesting(root/L"config.ini",false,
            [](HWND hwnd) { Check(SetTimer(hwnd,7,20,CompleteFixture)!=0,"fixture timer"); }); };
        Check(keyboard_app::RunApplication(root,layout,fixture,false)==0,"PASS must finish application with exit 0");
        const auto first=Read(root/L"KeyBoard.log");
        Check(first.find("Result: PASS")!=first.npos && first.find("keys passed 2/2")!=first.npos,"PASS summary must be flushed before return");
        closeTest=true;
        Check(keyboard_app::RunApplication(root,layout,fixture,false)==1,"manual close must fail");
        const auto second=Read(root/L"KeyBoard.log");
        Check(second.starts_with(first) && second.find("Result: FAIL")!=second.npos,"append must retain earlier run");
        std::filesystem::remove(root/L"config.ini");
        Check(keyboard_app::RunApplication(root,root/L"missing.KBL",{},false)==1,"missing KBL must fail without config.ini");
        Check(Read(root/L"KeyBoard.log").find("cannot open")!=std::string::npos,"KBL read failure must be logged");
        // A real failed append must never produce a success exit code.
        const auto blocked=root/L"blocked";
        std::filesystem::create_directories(blocked/L"KeyBoard.log");
        Check(keyboard_app::RunApplication(blocked,root/L"missing.KBL",{},false)==3,"log failure exit code");
        std::filesystem::remove_all(root);
        std::cout<<"Application tests passed: path resolution, PASS/exit/log, FAIL, append, missing KBL, log failure\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
