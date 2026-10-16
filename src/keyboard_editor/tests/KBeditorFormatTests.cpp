#include "KeyboardLayout.h"
#include "KeyboardLogic.h"
#include <iostream>
#include <stdexcept>

using namespace ftc::keyboard_editor;
void Check(bool value,const char* error) { if (!value) throw std::runtime_error(error); }
int main()
{
    try
    {
        KeyboardLayout layout; std::wstring error;
        Check(ParseLayout(L"1][986][197][1\r\nEsc][10][35][50][50][1][0\r\n",layout,error),"legacy import");
        Check(!SerializeLayout(layout).starts_with(L"KBL2"),"ordinary layout must stay legacy");
        layout.keys.push_back({L"Copilot",62,35,70,50,{{91,true,VK_LWIN},{42,false,VK_LSHIFT},{110,false,VK_F23}}});
        const auto serialized=SerializeLayout(layout);
        Check(serialized.starts_with(L"KBL2]["),"combination requires explicit v2");
        KeyboardLayout imported;
        Check(ParseLayout(serialized,imported,error),"combination import");
        Check(imported.keys[1].bindings==layout.keys[1].bindings && IsCopilot(imported.keys[1].bindings),"copilot binding roundtrip");
        Check(!ParseLayout(L"KBL2][1][100][100][1\nC][1][1][1][1][1][0][Chord][1,0,1+1,0,1\n",imported,error),"duplicate member");
        Check(!ParseLayout(L"KBL2][1][100][100][1\nC][1][1][1][1][1][0][Chord][1,0,1+1,0,2\n",imported,error),"same scan cannot become two chord members");
        const auto directory=std::filesystem::temp_directory_path()/(L"FTC-KBeditor-format-"+std::to_wstring(GetCurrentProcessId()));
        std::filesystem::create_directories(directory);
        const auto path=directory/L"布局.KBL";
        layout.keys[0].label=L"测试 Tiếng Việt";
        Check(WriteLayout(path,layout,error),"atomic first save");
        Check(ReadLayout(path,imported,error) && imported.keys[0].label==layout.keys[0].label,"UTF-8 label");
        layout.keys[0].label=L"Esc";
        Check(WriteLayout(path,layout,error),"atomic replacement");
        KeyboardLogic logic(imported);
        for (const auto& b:imported.keys[1].bindings) logic.Feed({b,true,100});
        for (const auto& b:imported.keys[1].bindings) logic.Feed({b,false,101});
        Check(logic.Passed()==1,"editor copilot preview");
        Check(BindingName({110,false,VK_F23})==L"F23","F23 name");
        std::filesystem::remove(path); std::filesystem::remove(directory);
        std::cout<<"KBeditorFormatTests passed\n"; return 0;
    }
    catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
