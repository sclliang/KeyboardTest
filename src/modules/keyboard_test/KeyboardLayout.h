#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

namespace ftc::modules::keyboard_test
{
struct Binding final
{
    unsigned scan{};
    bool extended{};
    unsigned vk{};
    bool operator==(const Binding&) const = default;
};
struct LayoutKey final
{
    std::wstring label;
    int x{}, y{}, width{50}, height{50};
    std::vector<Binding> bindings;
};
struct KeyboardLayout final
{
    int width{986}, height{300};
    bool stickCheck{true};
    std::vector<LayoutKey> keys;
};
bool ParseLayout(const std::wstring& text, KeyboardLayout& layout, std::wstring& error);
bool ReadLayout(const std::filesystem::path& path, KeyboardLayout& layout, std::wstring& error);
std::wstring SerializeLayout(const KeyboardLayout& layout);
bool WriteLayout(const std::filesystem::path& path, const KeyboardLayout& layout, std::wstring& error);
std::wstring BindingName(const Binding& binding);
std::wstring BindingDescription(const std::vector<Binding>& bindings);
bool IsCopilot(const std::vector<Binding>& bindings) noexcept;
}
