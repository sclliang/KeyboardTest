#pragma once
#include "modules/ModuleContract.h"
#include <filesystem>
namespace ftc::modules::keyboard_test
{
std::unique_ptr<ITestModule> CreateKeyboardTestModule(
    const std::filesystem::path& layout, const std::wstring& culture = L"zh-CN");
// Private fixture: real window and lifecycle, no global keyboard hook.
std::unique_ptr<ITestModule> CreateKeyboardTestModuleForTesting(
    const std::filesystem::path& config, bool showWindow, std::function<void(HWND)> onCreated = {});
inline constexpr UINT KeyboardFixtureEvent = WM_APP + 0x726;
inline constexpr UINT KeyboardFixtureRetest = WM_APP + 0x727;
inline constexpr UINT KeyboardFixtureDpi = WM_APP + 0x728;
}
