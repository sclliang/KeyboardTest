#include "KeyboardLayout.h"
#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>

namespace ftc::keyboard_editor
{
namespace
{
std::vector<std::wstring> Split(const std::wstring_view text, const std::wstring_view delimiter)
{
    std::vector<std::wstring> result;
    std::size_t start = 0;
    while (true)
    {
        const auto next = text.find(delimiter, start);
        result.emplace_back(text.substr(start, next == text.npos ? next : next - start));
        if (next == text.npos) return result;
        start = next + delimiter.size();
    }
}
bool Number(const std::wstring& text, unsigned& value)
{
    if (text.empty() || text.size() > 9) return false;
    value = 0;
    for (const wchar_t c : text)
    {
        if (c < L'0' || c > L'9') return false;
        value = value * 10U + static_cast<unsigned>(c - L'0');
    }
    return true;
}
bool Decode(const std::string& bytes, std::wstring& text)
{
    if (bytes.empty()) return false;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
        static_cast<unsigned char>(bytes[1]) == 0xFE)
    {
        if ((bytes.size() - 2) % 2) return false;
        text.resize((bytes.size() - 2) / 2);
        for (std::size_t i = 0; i < text.size(); ++i)
            text[i] = static_cast<wchar_t>(static_cast<unsigned char>(bytes[2 + i * 2]) |
                (static_cast<unsigned char>(bytes[3 + i * 2]) << 8));
        return true;
    }
    const auto offset = bytes.starts_with("\xEF\xBB\xBF") ? 3U : 0U;
    const char* input = bytes.data() + offset;
    const int size = static_cast<int>(bytes.size() - offset);
    UINT page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int length = MultiByteToWideChar(page, flags, input, size, nullptr, 0);
    if (!length)
    {
        if (offset) return false;
        page = CP_ACP; flags = 0;
        length = MultiByteToWideChar(page, flags, input, size, nullptr, 0);
    }
    if (!length) return false;
    text.resize(static_cast<std::size_t>(length));
    return MultiByteToWideChar(page, flags, input, size, text.data(), length) == length;
}
}
bool ParseLayout(const std::wstring& input, KeyboardLayout& layout, std::wstring& error)
{
    KeyboardLayout parsed;
    std::wistringstream stream(input);
    std::wstring line;
    if (!std::getline(stream, line)) { error = L"KBL: missing header"; return false; }
    if (!line.empty() && line.front() == 0xFEFF) line.erase(0, 1);
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    auto header = Split(line, L"][");
    const bool v2 = header.size() == 5 && _wcsicmp(header[0].c_str(), L"KBL2") == 0;
    if (v2) header.erase(header.begin());
    unsigned count{}, width{}, height{}, stick{};
    if (header.size() != 4 || !Number(header[0], count) || count == 0 || count > 512 ||
        !Number(header[1], width) || width < 32 || width > 16384 ||
        !Number(header[2], height) || height < 33 || height > 16384 || !Number(header[3], stick))
    { error = L"KBL: invalid header (count, width, height, Stick Key Test)"; return false; }
    parsed.width = static_cast<int>(width); parsed.height = static_cast<int>(height);
    parsed.stickCheck = stick != 0;
    for (unsigned index = 0; index < count; ++index)
    {
        const std::wstring prefix = L"KBL row " + std::to_wstring(index + 2) + L": ";
        if (!std::getline(stream, line)) { error = prefix + L"missing key"; return false; }
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        const auto fields = Split(line, L"][");
        if (fields.size() != (v2 ? 9U : 7U) || fields[0].empty() || fields[0].size() > 256)
        { error = prefix + L"invalid key fields"; return false; }
        unsigned values[6]{};
        for (unsigned i = 0; i < 6; ++i)
            if (!Number(fields[i + 1], values[i])) { error = prefix + L"invalid number"; return false; }
        if (values[0] > 16384 || values[1] > 16384 || values[2] == 0 || values[2] > 16384 ||
            values[3] == 0 || values[3] > 16384 || values[4] > 65535 || values[5] > 1)
        { error = prefix + L"invalid geometry / scan / extended flag"; return false; }
        LayoutKey key{fields[0], static_cast<int>(values[0]), static_cast<int>(values[1]),
            static_cast<int>(values[2]), static_cast<int>(values[3]), {{values[4], values[5] != 0, 0}}};
        if (v2)
        {
            const bool single = _wcsicmp(fields[7].c_str(), L"Single") == 0;
            const bool chord = _wcsicmp(fields[7].c_str(), L"Chord") == 0;
            key.bindings.clear();
            const auto members = Split(fields[8], L"+");
            if ((!single && !chord) || (single && members.size() != 1) ||
                (chord && (members.size() < 2 || members.size() > 8)))
            { error = prefix + L"invalid binding type"; return false; }
            for (const auto& member : members)
            {
                const auto parts = Split(member, L",");
                unsigned scan{}, ext{}, vk{};
                if (parts.size() != 3 || !Number(parts[0], scan) || scan > 65535 ||
                    !Number(parts[1], ext) || ext > 1 || !Number(parts[2], vk) || vk > 255 ||
                    (scan == 0 && vk == 0))
                { error = prefix + L"invalid binding"; return false; }
                const Binding binding{scan, ext != 0, vk};
                if (std::any_of(key.bindings.begin(),key.bindings.end(),[&](const Binding& previous) {
                    return binding.scan ? previous.scan==binding.scan && previous.extended==binding.extended :
                        previous.scan==0 && previous.vk==binding.vk;
                }))
                { error = prefix + L"duplicate chord member"; return false; }
                key.bindings.push_back(binding);
            }
        }
        parsed.keys.push_back(std::move(key));
    }
    while (std::getline(stream, line))
        if (line.find_first_not_of(L" \t\r") != line.npos)
        { error = L"KBL: unexpected trailing records"; return false; }
    layout = std::move(parsed); error.clear(); return true;
}
bool ReadLayout(const std::filesystem::path& path, KeyboardLayout& layout, std::wstring& error)
{
    try
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) { error = L"KBL: cannot open " + path.wstring(); return false; }
        const auto size = file.tellg();
        if (size <= 0 || size > 1024 * 1024) { error = L"KBL: empty or too large (>1 MiB)"; return false; }
        std::string bytes(static_cast<std::size_t>(size), '\0');
        file.seekg(0); file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        std::wstring decoded;
        if (!file || !Decode(bytes, decoded)) { error = L"KBL: read / encoding failed"; return false; }
        return ParseLayout(decoded, layout, error);
    }
    catch (...) { error = L"KBL: file read failed"; return false; }
}
std::wstring SerializeLayout(const KeyboardLayout& layout)
{
    const bool v2 = std::any_of(layout.keys.begin(), layout.keys.end(), [](const auto& key) {
        return key.bindings.size() != 1 || key.bindings.front().scan == 0;
    });
    std::wostringstream out;
    if (v2) out << L"KBL2][";
    out << layout.keys.size() << L"][" << layout.width << L"][" << layout.height << L"]["
        << (layout.stickCheck ? 1 : 0) << L"\r\n";
    for (const auto& key : layout.keys)
    {
        const Binding first = key.bindings.empty() ? Binding{} : key.bindings.front();
        out << key.label << L"][" << key.x << L"][" << key.y << L"][" << key.width << L"]["
            << key.height << L"][" << first.scan << L"][" << (first.extended ? 1 : 0);
        if (v2)
        {
            out << L"][" << (key.bindings.size() == 1 ? L"Single" : L"Chord") << L"][";
            bool separator = false;
            for (const auto& binding : key.bindings)
            {
                if (separator) out << L"+";
                out << binding.scan << L"," << (binding.extended ? 1 : 0) << L"," << binding.vk;
                separator = true;
            }
        }
        out << L"\r\n";
    }
    return out.str();
}
bool WriteLayout(const std::filesystem::path& path, const KeyboardLayout& layout, std::wstring& error)
{
    // Validate the exact serialized document before touching the destination.
    const auto text = SerializeLayout(layout);
    KeyboardLayout checked;
    if (!ParseLayout(text, checked, error)) return false;
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!length) { error = L"KBL: invalid Unicode label"; return false; }
    std::string bytes(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
        bytes.data(), length, nullptr, nullptr);
    std::filesystem::path temporary = path;
    temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." + std::to_wstring(GetTickCount64()) + L".tmp";
    const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) { error = L"CreateFile(KBL temp): " + std::to_wstring(GetLastError()); return false; }
    DWORD written{};
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) &&
        written == bytes.size() && FlushFileBuffers(file);
    const DWORD code = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!ok || !MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        error = L"Write/replace KBL: " + std::to_wstring(ok ? GetLastError() : code);
        DeleteFileW(temporary.c_str()); return false;
    }
    error.clear(); return true;
}
std::wstring BindingName(const Binding& binding)
{
    const unsigned vk = binding.vk ? binding.vk : MapVirtualKeyW(
        binding.scan | (binding.extended ? 0xE000U : 0U), MAPVK_VSC_TO_VK_EX);
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    switch (vk)
    {
    case VK_LWIN: return L"Win (L)"; case VK_RWIN: return L"Win (R)";
    case VK_LSHIFT: return L"Shift (L)"; case VK_RSHIFT: return L"Shift (R)";
    case VK_LCONTROL: return L"Ctrl (L)"; case VK_RCONTROL: return L"Ctrl (R)";
    case VK_LMENU: return L"Alt (L)"; case VK_RMENU: return L"Alt (R)";
    case VK_PAUSE: return L"Pause";
    default: break;
    }
    const unsigned mapped = binding.scan ? binding.scan | (binding.extended ? 0xE000U : 0U) : MapVirtualKeyW(vk,MAPVK_VK_TO_VSC_EX);
    wchar_t name[128]{};
    if (GetKeyNameTextW(static_cast<LONG>(((mapped & 0xFFU) << 16) | ((mapped & 0xFF00U) ? (1U << 24) : 0)), name, 128))
        return name;
    if (!binding.scan && vk) return L"VK " + std::to_wstring(vk);
    return L"Scan " + std::to_wstring(binding.scan) + (binding.extended ? L" (E0)" : L"");
}
std::wstring BindingDescription(const std::vector<Binding>& bindings)
{
    std::wstring out;
    for (const auto& binding : bindings)
    {
        if (!out.empty()) out += L" + ";
        out += BindingName(binding) + L" [" + std::to_wstring(binding.scan) + L"," +
            (binding.extended ? L"1" : L"0") + (binding.scan ? L"" : L",VK="+std::to_wstring(binding.vk)) + L"]";
    }
    return out;
}
bool IsCopilot(const std::vector<Binding>& bindings) noexcept
{
    bool win = false, shift = false, f23 = false, c = false;
    for (const auto& binding : bindings)
    {
        const unsigned vk = binding.vk ? binding.vk : MapVirtualKeyW(
            binding.scan | (binding.extended ? 0xE000U : 0U), MAPVK_VSC_TO_VK_EX);
        win |= vk == VK_LWIN || vk == VK_RWIN;
        shift |= vk == VK_LSHIFT; f23 |= vk == VK_F23; c |= vk == 'C';
    }
    return (bindings.size() == 3 && win && shift && f23) || (bindings.size() == 2 && win && c);
}
}
