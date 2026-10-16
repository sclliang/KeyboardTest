#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ftc::modules
{
enum class IniNameComparison
{
    Exact,
    AsciiCaseInsensitive,
};

// Project configuration lookups default to ASCII case-insensitive section
// and key names. Exact matching remains an explicit opt-in for a future value
// whose spelling is required by a user-confirmed compatibility rule.

[[nodiscard]] bool IniNameEquals(
    std::wstring_view left,
    std::wstring_view right,
    IniNameComparison comparison) noexcept;

struct IniEntry final
{
    std::wstring key;
    std::wstring value;
    std::size_t sourceLine{};
};

struct IniSection final
{
    std::wstring name;
    std::size_t sourceLine{};
    std::vector<IniEntry> entries;

    [[nodiscard]] const IniEntry* FindLastEntry(
        std::wstring_view key,
        IniNameComparison comparison = IniNameComparison::AsciiCaseInsensitive) const noexcept;
};

struct IniDocument final
{
    // Original section and entry order, including repeated sections/keys, is
    // preserved so each module can apply its own compatibility semantics.
    std::vector<IniSection> sections;

    [[nodiscard]] const IniSection* FindLastSection(
        std::wstring_view name,
        IniNameComparison comparison = IniNameComparison::AsciiCaseInsensitive) const noexcept;
};

struct ModuleConfigLoadResult;

class ModuleConfig final
{
public:
    [[nodiscard]] std::wstring GetValue(
        std::wstring_view section,
        std::wstring_view key,
        std::wstring_view defaultValue = {},
        IniNameComparison sectionComparison = IniNameComparison::AsciiCaseInsensitive,
        IniNameComparison keyComparison = IniNameComparison::AsciiCaseInsensitive) const;

    [[nodiscard]] const IniSection* GetSection(
        std::wstring_view section,
        IniNameComparison comparison = IniNameComparison::AsciiCaseInsensitive) const noexcept;

    [[nodiscard]] const std::vector<IniSection>& Sections() const noexcept;
    [[nodiscard]] const std::filesystem::path& SourcePath() const noexcept;
    [[nodiscard]] bool UsedAnsiFallback() const noexcept;

private:
    ModuleConfig() = default;

    friend struct ModuleConfigLoadResult;
    friend ModuleConfigLoadResult LoadModuleConfig(const std::filesystem::path& path);
    friend ModuleConfigLoadResult LoadEncryptedModuleConfig(
        const std::filesystem::path& path);

    std::filesystem::path sourcePath_;
    IniDocument document_;
    bool usedAnsiFallback_{};
};

struct ModuleConfigLoadResult final
{
    bool success{false};
    std::shared_ptr<const ModuleConfig> config;
    std::wstring error;
};

// Shared infrastructure only: reads bytes, decodes text, and preserves the
// WPF FILE object's INI syntax. It does not define module defaults, validate
// business values, split module fields, or decide PASS/FAIL.
[[nodiscard]] ModuleConfigLoadResult LoadModuleConfig(
    const std::filesystem::path& path);

// Production compatibility entry point. The file must be the same raw
// AES-128-CBC/PKCS#7 ciphertext consumed by the legacy WPF application.
// Decryption is performed in memory and never creates temp.ini.
[[nodiscard]] ModuleConfigLoadResult LoadEncryptedModuleConfig(
    const std::filesystem::path& path);

// The application calls this once during startup. It accepts only the legacy
// encrypted config.ini format. Every production module then obtains the same
// immutable in-memory object; no test run reopens or decodes config.ini.
[[nodiscard]] ModuleConfigLoadResult InitializeGlobalModuleConfig(
    const std::filesystem::path& path);
[[nodiscard]] std::shared_ptr<const ModuleConfig> GetGlobalModuleConfig();
[[nodiscard]] std::wstring GetGlobalModuleConfigError();
} // namespace ftc::modules
