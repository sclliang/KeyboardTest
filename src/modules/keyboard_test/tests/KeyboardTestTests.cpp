#include "modules/keyboard_test/KeyboardTestModule.h"
#include "modules/keyboard_test/KeyboardLayout.h"
#include "modules/keyboard_test/KeyboardLogic.h"
#include <atomic>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace ftc::modules;
using namespace ftc::modules::keyboard_test;
void Check(bool value,const char* error) { if (!value) throw std::runtime_error(error); }
KeyboardLayout Example()
{
    KeyboardLayout layout; layout.stickCheck=false;
    layout.keys={{L"Win",10,35,50,50,{{91,true,0}}},{L"Shift",62,35,50,50,{{42,false,0}}},
        {L"F23",114,35,50,50,{{110,false,0}}},{L"Copilot",166,35,70,50,{{91,true,VK_LWIN},{42,false,VK_LSHIFT},{110,false,VK_F23}}}};
    return layout;
}
void LogicTests()
{
    auto layout=Example(); KeyboardLogic logic(layout);
    logic.Feed({{91,true,VK_LWIN},false,1}); Check(logic.Passed()==0,"orphan release must not pass");
    logic.Feed({{91,true,VK_LWIN},true,2}); logic.Feed({{91,true,VK_LWIN},true,3});
    Check(logic.Passed()==0,"auto-repeat cannot complete a key");
    logic.Feed({{91,true,VK_LWIN},false,4}); Check(logic.Passed()==1,"valid standalone Win");
    logic.Reset();
    for (const auto& b:layout.keys[3].bindings) logic.Feed({b,true,10});
    Check(logic.Passed()==0,"chord must wait for release");
    logic.Feed({layout.keys[3].bindings[2],false,11}); Check(logic.Passed()==0,"partial release");
    logic.Feed({layout.keys[3].bindings[0],false,12}); logic.Feed({layout.keys[3].bindings[1],false,13});
    Check(logic.Passed()==1 && logic.Status()[3]==KeyStatus::Passed,"Copilot counts exactly one");
    for (std::size_t i=0;i<3;++i) Check(logic.Status()[i]!=KeyStatus::Passed,"composite must not pass constituent keys");
    logic.Reset();
    logic.Feed({layout.keys[3].bindings[0],true,10}); logic.Suspend();
    logic.Feed({layout.keys[3].bindings[0],false,11}); Check(logic.Passed()==0,"blur prevents held-key pass");
    layout=Example(); layout.stickCheck=true; logic=KeyboardLogic(layout);
    for (const auto& b:layout.keys[3].bindings) logic.Feed({b,true,100});
    for (const auto& b:layout.keys[3].bindings) logic.Feed({b,false,101});
    Check(logic.Passed()==1,"20 ms must not reject internal Copilot sequence");
    layout.keys={{L"A",10,35,50,50,{{30,false,'A'}}},{L"B",62,35,50,50,{{48,false,'B'}}}};
    logic=KeyboardLogic(layout); logic.Feed({{30,false,'A'},true,100}); logic.Feed({{48,false,'B'},true,105});
    logic.Feed({{48,false,'B'},false,110}); logic.Feed({{30,false,'A'},false,115});
    Check(logic.Passed()==0 && logic.Status()[0]==KeyStatus::Retry,"legacy rapid adjacency retry");
    logic.Reset(); logic.Feed({{30,false,'A'},true,200}); logic.Feed({{30,false,'A'},false,210});
    logic.Feed({{48,false,'B'},true,250}); logic.Feed({{48,false,'B'},false,260});
    Check(logic.Complete(),"retest clears all timing / input state");
    layout.keys={{L"Pause",0,0,50,50,{{69,false,VK_PAUSE}}}}; logic=KeyboardLogic(layout);
    logic.Feed({{69,false,VK_PAUSE},true,0}); Check(logic.Complete(),"Pause down-only input");
    layout=Example(); layout.keys[3].bindings={{91,true,VK_LWIN},{46,false,'C'}};
    Check(IsCopilot(layout.keys[3].bindings),"Win+C profile");
    KeyboardLayout parsed; std::wstring error;
    Check(ParseLayout(L"1][986][197][1\r\nEsc][10][35][50][50][1][0\r\n",parsed,error),"legacy Main.KBL fields");
    Check(ParseLayout(SerializeLayout(layout),parsed,error),"V2 roundtrip");
    Check(!ParseLayout(L"1][10][10][1\n",parsed,error),"truncated file");
    Check(!ParseLayout(L"KBL2][1][100][100][1\nC][1][1][1][1][1][0][Chord][1,0,1+1,0,2\n",parsed,error),"same physical event cannot be two chord members");
}
void PumpUntil(const std::function<bool()>& done)
{
    const auto end=GetTickCount64()+5000;
    while (!done() && GetTickCount64()<end)
    {
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        MsgWaitForMultipleObjectsEx(0,nullptr,10,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
    }
    Check(done(),"window lifecycle timeout");
}
void LifecycleTests()
{
    const auto directory=std::filesystem::temp_directory_path()/(L"FTC-KeyboardTest-"+std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(directory/L"ExternalTools");
    std::wstring error; auto layout=Example(); layout.keys={layout.keys.back()};
    Check(WriteLayout(directory/L"ExternalTools/Main.KBL",layout,error),"fixture layout save");
    { std::ofstream config(directory/L"config.ini"); config<<"[LANGUAGE]\nCULTURE=EN-us\n[KEYBOARDTEST]\nLAYOUTFILE=ExternalTools/Main.KBL\n"; }
    for (const TestResult expected:{TestResult::Pass,TestResult::Fail,TestResult::Cancel})
    {
        HWND window{}; std::atomic<unsigned> completions{}; std::atomic_bool closedAtReport{};
        auto module=CreateKeyboardTestModuleForTesting(directory/L"config.ini",false,[&](HWND hwnd){window=hwnd;});
        ModuleCallbacks callbacks; callbacks.onCompletion=[&](Completion completion) {
            closedAtReport.store(!IsWindow(window)); Check(completion.result==expected,"wrong terminal result"); ++completions;
        };
        Check(module->Start(nullptr,std::move(callbacks)).IsStarted(),"module fixture Start");
        PumpUntil([&]{return IsWindowEnabled(GetDlgItem(window,101))!=FALSE;});
        SendMessageW(window,KeyboardFixtureRetest,0,0);
        if (expected==TestResult::Pass)
        {
            for (const bool down:{true,false}) for (const auto& b:layout.keys[0].bindings)
                SendMessageW(window,KeyboardFixtureEvent,down ? 1 : 0,
                    static_cast<LPARAM>(b.scan|(b.extended ? 0x10000 : 0)|(b.vk<<17)));
        }
        else if (expected==TestResult::Fail) SendMessageW(window,WM_CLOSE,0,0);
        else { module->RequestStop(); module->RequestStop(); }
        PumpUntil([&]{return completions.load()!=0;});
        Check(completions==1 && closedAtReport,"exactly one result after window cleanup");
        module.reset();
    }
    std::filesystem::remove(directory/L"config.ini"); std::filesystem::remove(directory/L"ExternalTools/Main.KBL");
    std::filesystem::remove(directory/L"ExternalTools"); std::filesystem::remove(directory);
}
void Visual(const std::filesystem::path& directory, const std::wstring& onlyLanguage)
{
    std::filesystem::create_directories(directory/L"ExternalTools");
    auto layout=Example();
    layout.keys.push_back({L"Esc",10,98,50,50,{{1,false,VK_ESCAPE}}});
    layout.keys.push_back({L"Enter",62,98,102,50,{{28,false,VK_RETURN}}});
    layout.width=650; layout.height=260; std::wstring error;
    Check(WriteLayout(directory/L"ExternalTools/Main.KBL",layout,error),"visual layout");
    for (const auto* language:{"zh-CN","en-US","vi-VN"})
    {
        const std::wstring culture(language,language+strlen(language));
        if (!onlyLanguage.empty() && _wcsicmp(culture.c_str(),onlyLanguage.c_str())!=0) continue;
        { std::ofstream config(directory/L"config.ini"); config<<"[language]\nCulture="<<language<<"\n"; }
        HWND window{}; std::atomic_bool completed{};
        auto module=CreateKeyboardTestModuleForTesting(directory/L"config.ini",true,[&](HWND hwnd){window=hwnd;});
        ModuleCallbacks callbacks; callbacks.onCompletion=[&](Completion) { completed=true; };
        Check(module->Start(nullptr,std::move(callbacks)).IsStarted(),"visual Start");
        PumpUntil([&]{return IsWindowEnabled(GetDlgItem(window,101))!=FALSE;});
        std::cout<<"Visual "<<language<<": actual DPI="<<GetDpiForWindow(window)<<'\n'<<std::flush;
        for (const bool down:{true,false}) for (const auto& b:layout.keys[3].bindings)
            SendMessageW(window,KeyboardFixtureEvent,down ? 1 : 0,static_cast<LPARAM>(b.scan|(b.extended ? 0x10000 : 0)|(b.vk<<17)));
        const auto end=GetTickCount64()+60000;
        while (!completed && GetTickCount64()<end)
        {
            MSG message{}; while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
            MsgWaitForMultipleObjectsEx(0,nullptr,20,QS_ALLINPUT,MWMO_INPUTAVAILABLE);
        }
        module->RequestStop(); PumpUntil([&]{return completed.load();});
    }
}
int wmain(int argc,wchar_t** argv)
{
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try
    {
        if (argc>2 && std::wstring_view(argv[1])==L"--visual")
        { Visual(std::filesystem::absolute(argv[2]),argc>3 ? argv[3] : L""); return 0; }
        LogicTests(); LifecycleTests(); std::cout<<"KeyboardTestTests passed\n"; return 0;
    }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
