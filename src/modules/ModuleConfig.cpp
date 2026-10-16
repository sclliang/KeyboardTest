#include "modules/ModuleConfig.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace ftc::modules
{
namespace
{
constexpr std::array<unsigned char, 16U> kLegacyAesKey{
    '0', '1', '2', '3', '4', '5', '6', '7',
    '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
constexpr std::array<unsigned char, 16U> kLegacyAesIv{
    'F', 'E', 'D', 'C', 'B', 'A', '9', '8',
    '7', '6', '5', '4', '3', '2', '1', '0'};

struct AlgorithmHandle final
{
    BCRYPT_ALG_HANDLE value{};
    ~AlgorithmHandle()
    {
        if (value != nullptr)
        {
            static_cast<void>(BCryptCloseAlgorithmProvider(value, 0U));
        }
    }
};

struct KeyHandle final
{
    BCRYPT_KEY_HANDLE value{};
    ~KeyHandle()
    {
        if (value != nullptr)
        {
            static_cast<void>(BCryptDestroyKey(value));
        }
    }
};

struct GlobalConfigState final
{
    std::mutex mutex;
    bool initialized{false};
    std::filesystem::path sourcePath;
    ModuleConfigLoadResult result;
};

[[nodiscard]] GlobalConfigState& GetGlobalConfigState()
{
    static GlobalConfigState state;
    return state;
}

[[nodiscard]] std::wstring CryptoError(
    const std::wstring_view stage,
    const NTSTATUS status)
{
    wchar_t code[16]{};
    static_cast<void>(swprintf_s(
        code,
        L"0x%08X",
        static_cast<unsigned int>(status)));
    return std::wstring(stage) + L"失败（" + code + L"）。";
}

[[nodiscard]] bool DecryptLegacyWpfConfig(
    const std::vector<char>& cipherText,
    std::vector<char>& plainText,
    std::wstring& error)
{
    plainText.clear();
    if (cipherText.empty() || (cipherText.size() % 16U) != 0U)
    {
        error = L"config.ini 长度无效，不是旧 WPF AES-CBC 密文。";
        return false;
    }
    if (cipherText.size() > static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()))
    {
        error = L"config.ini 太大，无法进行 AES 解密。";
        return false;
    }

    AlgorithmHandle algorithm;
    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &algorithm.value,
        BCRYPT_AES_ALGORITHM,
        nullptr,
        0U);
    if (!BCRYPT_SUCCESS(status))
    {
        error = CryptoError(L"初始化 AES", status);
        return false;
    }

    status = BCryptSetProperty(
        algorithm.value,
        BCRYPT_CHAINING_MODE,
        reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_CBC)),
        static_cast<ULONG>(sizeof(BCRYPT_CHAIN_MODE_CBC)),
        0U);
    if (!BCRYPT_SUCCESS(status))
    {
        error = CryptoError(L"设置 AES-CBC 模式", status);
        return false;
    }

    ULONG keyObjectSize = 0U;
    ULONG received = 0U;
    status = BCryptGetProperty(
        algorithm.value,
        BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&keyObjectSize),
        sizeof(keyObjectSize),
        &received,
        0U);
    if (!BCRYPT_SUCCESS(status))
    {
        error = CryptoError(L"读取 AES 密钥参数", status);
        return false;
    }

    std::vector<unsigned char> keyObject(keyObjectSize);
    std::array<unsigned char, kLegacyAesKey.size()> keyBytes = kLegacyAesKey;
    KeyHandle key;
    status = BCryptGenerateSymmetricKey(
        algorithm.value,
        &key.value,
        keyObject.data(),
        static_cast<ULONG>(keyObject.size()),
        keyBytes.data(),
        static_cast<ULONG>(keyBytes.size()),
        0U);
    SecureZeroMemory(keyBytes.data(), keyBytes.size());
    if (!BCRYPT_SUCCESS(status))
    {
        error = CryptoError(L"创建 AES 密钥", status);
        return false;
    }

    const ULONG inputSize = static_cast<ULONG>(cipherText.size());
    auto* const input = reinterpret_cast<PUCHAR>(const_cast<char*>(cipherText.data()));
    std::array<unsigned char, kLegacyAesIv.size()> iv = kLegacyAesIv;
    ULONG outputSize = 0U;
    status = BCryptDecrypt(
        key.value,
        input,
        inputSize,
        nullptr,
        iv.data(),
        static_cast<ULONG>(iv.size()),
        nullptr,
        0U,
        &outputSize,
        BCRYPT_BLOCK_PADDING);
    if (!BCRYPT_SUCCESS(status))
    {
        error = CryptoError(L"计算 config.ini 解密长度", status);
        return false;
    }

    plainText.resize(outputSize);
    iv = kLegacyAesIv;
    ULONG written = 0U;
    status = BCryptDecrypt(
        key.value,
        input,
        inputSize,
        nullptr,
        iv.data(),
        static_cast<ULONG>(iv.size()),
        reinterpret_cast<PUCHAR>(plainText.data()),
        outputSize,
        &written,
        BCRYPT_BLOCK_PADDING);
    if (!BCRYPT_SUCCESS(status))
    {
        plainText.clear();
        error = CryptoError(L"解密 config.ini", status);
        return false;
    }
    plainText.resize(written);
    return true;
}

[[nodiscard]] bool IsSpace(const wchar_t value) noexcept
{
    return (value >= L'\t' && value <= L'\r') || value == L' ' ||
           value == L'\x0085' || value == L'\x00A0' || value == L'\x1680' ||
           (value >= L'\x2000' && value <= L'\x200A') ||
           value == L'\x2028' || value == L'\x2029' || value == L'\x202F' ||
           value == L'\x205F' || value == L'\x3000';
}

[[nodiscard]] std::wstring_view Trim(const std::wstring_view value) noexcept
{
    std::size_t first = 0U;
    while (first < value.size() && IsSpace(value[first]))
    {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first && IsSpace(value[last - 1U]))
    {
        --last;
    }
    return value.substr(first, last - first);
}

[[nodiscard]] wchar_t FoldAscii(const wchar_t value) noexcept
{
    if (value >= L'A' && value <= L'Z')
    {
        return static_cast<wchar_t>(value + (L'a' - L'A'));
    }
    return value;
}

[[nodiscard]] bool ConvertMultiByte(
    const std::string_view bytes,
    const UINT codePage,
    const DWORD flags,
    std::wstring& text)
{
    if (bytes.empty())
    {
        text.clear();
        return true;
    }
    if (bytes.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return false;
    }

    const int byteCount = static_cast<int>(bytes.size());
    const int characterCount =
        MultiByteToWideChar(codePage, flags, bytes.data(), byteCount, nullptr, 0);
    if (characterCount <= 0)
    {
        return false;
    }

    std::wstring converted(static_cast<std::size_t>(characterCount), L'\0');
    const int convertedCount = MultiByteToWideChar(
        codePage,
        flags,
        bytes.data(),
        byteCount,
        converted.data(),
        characterCount);
    if (convertedCount != characterCount)
    {
        return false;
    }

    text = std::move(converted);
    return true;
}

[[nodiscard]] bool DecodeUtf16LittleEndian(
    const std::string_view bytes,
    std::wstring& text)
{
    static_assert(sizeof(wchar_t) == sizeof(std::uint16_t));
    if ((bytes.size() % 2U) != 0U)
    {
        return false;
    }

    std::wstring converted;
    converted.reserve(bytes.size() / 2U);
    bool waitingForLowSurrogate = false;
    for (std::size_t index = 0U; index < bytes.size(); index += 2U)
    {
        const auto low = static_cast<unsigned char>(bytes[index]);
        const auto high = static_cast<unsigned char>(bytes[index + 1U]);
        const std::uint16_t codeUnit = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(low) |
            static_cast<std::uint16_t>(static_cast<std::uint16_t>(high) << 8U));

        const bool highSurrogate = codeUnit >= 0xD800U && codeUnit <= 0xDBFFU;
        const bool lowSurrogate = codeUnit >= 0xDC00U && codeUnit <= 0xDFFFU;
        if ((waitingForLowSurrogate && !lowSurrogate) ||
            (!waitingForLowSurrogate && lowSurrogate))
        {
            return false;
        }
        waitingForLowSurrogate = highSurrogate;
        if (lowSurrogate)
        {
            waitingForLowSurrogate = false;
        }
        converted.push_back(static_cast<wchar_t>(codeUnit));
    }
    if (waitingForLowSurrogate)
    {
        return false;
    }

    text = std::move(converted);
    return true;
}

[[nodiscard]] bool DecodeIniBytes(
    const std::vector<char>& bytes,
    std::wstring& text,
    bool& usedAnsiFallback,
    std::wstring& error)
{
    usedAnsiFallback = false;
    const std::string_view allBytes = bytes.empty()
        ? std::string_view{}
        : std::string_view(bytes.data(), bytes.size());

    if (bytes.size() >= 2U && static_cast<unsigned char>(bytes[0]) == 0xFFU &&
        static_cast<unsigned char>(bytes[1]) == 0xFEU)
    {
        if (DecodeUtf16LittleEndian(allBytes.substr(2U), text))
        {
            return true;
        }
        error = L"INI 文件的 UTF-16LE 内容无效。";
        return false;
    }

    if (bytes.size() >= 2U && static_cast<unsigned char>(bytes[0]) == 0xFEU &&
        static_cast<unsigned char>(bytes[1]) == 0xFFU)
    {
        error = L"INI 文件使用了不支持的 UTF-16BE 编码。";
        return false;
    }

    std::size_t utf8Offset = 0U;
    if (bytes.size() >= 3U && static_cast<unsigned char>(bytes[0]) == 0xEFU &&
        static_cast<unsigned char>(bytes[1]) == 0xBBU &&
        static_cast<unsigned char>(bytes[2]) == 0xBFU)
    {
        utf8Offset = 3U;
    }

    const std::string_view content = allBytes.substr(utf8Offset);
    if (ConvertMultiByte(content, CP_UTF8, MB_ERR_INVALID_CHARS, text))
    {
        return true;
    }
    if (ConvertMultiByte(content, CP_ACP, 0U, text))
    {
        usedAnsiFallback = true;
        return true;
    }

    error = L"INI 文件无法按 UTF-8 或系统 ANSI 代码页解码。";
    return false;
}

[[nodiscard]] IniDocument ParseIniText(const std::wstring& text)
{
    IniDocument document;
    std::optional<std::size_t> currentSection;
    std::size_t sourceLine = 1U;
    std::size_t start = 0U;

    while (start <= text.size())
    {
        const std::size_t newline = text.find_first_of(L"\r\n", start);
        const std::size_t end = newline == std::wstring::npos ? text.size() : newline;
        const std::wstring_view line =
            Trim(std::wstring_view(text).substr(start, end - start));

        if (!line.empty() && line.front() != L';' && line.front() != L'#')
        {
            if (line.front() == L'[' && line.size() >= 2U && line.back() == L']')
            {
                // Match the legacy WPF FILE object: outer whitespace is
                // trimmed, but whitespace inside the brackets is significant.
                const std::wstring_view name = line.substr(1U, line.size() - 2U);
                document.sections.push_back(
                    IniSection{std::wstring(name), sourceLine, {}});
                if (name.empty())
                {
                    currentSection.reset();
                }
                else
                {
                    currentSection = document.sections.size() - 1U;
                }
            }
            else if (currentSection.has_value())
            {
                const std::size_t equals = line.find(L'=');
                if (equals != std::wstring_view::npos)
                {
                    const std::wstring_view key = Trim(line.substr(0U, equals));
                    const std::wstring_view value = Trim(line.substr(equals + 1U));
                    document.sections[*currentSection].entries.push_back(
                        IniEntry{std::wstring(key), std::wstring(value), sourceLine});
                }
            }
        }

        if (newline == std::wstring::npos)
        {
            break;
        }
        start = newline + 1U;
        if (text[newline] == L'\r' && start < text.size() && text[start] == L'\n')
        {
            ++start;
        }
        ++sourceLine;
    }
    return document;
}

[[nodiscard]] std::wstring ExceptionText(const char* const message)
{
    if (message == nullptr || *message == '\0')
    {
        return L"未知错误";
    }

    std::wstring converted;
    if (ConvertMultiByte(message, CP_UTF8, MB_ERR_INVALID_CHARS, converted) ||
        ConvertMultiByte(message, CP_ACP, 0U, converted))
    {
        return converted;
    }
    return L"未知错误";
}

[[nodiscard]] bool PathsEqual(
    const std::filesystem::path& left,
    const std::filesystem::path& right) noexcept
{
    const std::wstring& leftText = left.native();
    const std::wstring& rightText = right.native();
    if (leftText.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()) ||
        rightText.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
    {
        return false;
    }
    return CompareStringOrdinal(
               leftText.data(),
               static_cast<int>(leftText.size()),
               rightText.data(),
               static_cast<int>(rightText.size()),
               TRUE) == CSTR_EQUAL;
}
} // namespace

bool IniNameEquals(
    const std::wstring_view left,
    const std::wstring_view right,
    const IniNameComparison comparison) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    if (comparison == IniNameComparison::Exact)
    {
        return left == right;
    }

    for (std::size_t index = 0U; index < left.size(); ++index)
    {
        if (FoldAscii(left[index]) != FoldAscii(right[index]))
        {
            return false;
        }
    }
    return true;
}

const IniEntry* IniSection::FindLastEntry(
    const std::wstring_view key,
    const IniNameComparison comparison) const noexcept
{
    for (auto iterator = entries.crbegin(); iterator != entries.crend(); ++iterator)
    {
        if (IniNameEquals(iterator->key, key, comparison))
        {
            return &*iterator;
        }
    }
    return nullptr;
}

const IniSection* IniDocument::FindLastSection(
    const std::wstring_view name,
    const IniNameComparison comparison) const noexcept
{
    for (auto iterator = sections.crbegin(); iterator != sections.crend(); ++iterator)
    {
        if (IniNameEquals(iterator->name, name, comparison))
        {
            return &*iterator;
        }
    }
    return nullptr;
}

std::wstring ModuleConfig::GetValue(
    const std::wstring_view section,
    const std::wstring_view key,
    const std::wstring_view defaultValue,
    const IniNameComparison sectionComparison,
    const IniNameComparison keyComparison) const
{
    const IniSection* const foundSection =
        document_.FindLastSection(section, sectionComparison);
    const IniEntry* const foundEntry = foundSection == nullptr
        ? nullptr
        : foundSection->FindLastEntry(key, keyComparison);
    return foundEntry == nullptr ? std::wstring(defaultValue) : foundEntry->value;
}

const IniSection* ModuleConfig::GetSection(
    const std::wstring_view section,
    const IniNameComparison comparison) const noexcept
{
    return document_.FindLastSection(section, comparison);
}

const std::vector<IniSection>& ModuleConfig::Sections() const noexcept
{
    return document_.sections;
}

const std::filesystem::path& ModuleConfig::SourcePath() const noexcept
{
    return sourcePath_;
}

bool ModuleConfig::UsedAnsiFallback() const noexcept
{
    return usedAnsiFallback_;
}

ModuleConfigLoadResult LoadModuleConfig(const std::filesystem::path& path)
{
    ModuleConfigLoadResult result;
    try
    {
        if (path.empty() || !path.is_absolute())
        {
            result.error = L"INI 配置路径必须是绝对路径。";
            return result;
        }

        const std::filesystem::path normalizedPath = path.lexically_normal();
        std::ifstream input(normalizedPath, std::ios::binary);
        if (!input.is_open())
        {
            result.error = L"无法打开 INI 配置文件：" + normalizedPath.wstring();
            return result;
        }

        const std::vector<char> bytes{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        if (input.bad())
        {
            result.error = L"读取 INI 配置文件失败：" + normalizedPath.wstring();
            return result;
        }

        std::wstring decodedText;
        bool usedAnsiFallback = false;
        if (!DecodeIniBytes(bytes, decodedText, usedAnsiFallback, result.error))
        {
            return result;
        }

        std::shared_ptr<ModuleConfig> config(new ModuleConfig());
        config->sourcePath_ = normalizedPath;
        config->document_ = ParseIniText(decodedText);
        config->usedAnsiFallback_ = usedAnsiFallback;

        result.config = std::move(config);
        result.success = true;
        return result;
    }
    catch (const std::exception& exception)
    {
        result.error = L"读取 INI 配置文件时发生异常：" +
            ExceptionText(exception.what());
        return result;
    }
    catch (...)
    {
        result.error = L"读取 INI 配置文件时发生未知异常。";
        return result;
    }
}

ModuleConfigLoadResult LoadEncryptedModuleConfig(const std::filesystem::path& path)
{
    ModuleConfigLoadResult result;
    try
    {
        if (path.empty() || !path.is_absolute())
        {
            result.error = L"INI 配置路径必须是绝对路径。";
            return result;
        }

        const std::filesystem::path normalizedPath = path.lexically_normal();
        std::ifstream input(normalizedPath, std::ios::binary);
        if (!input.is_open())
        {
            result.error = L"无法打开加密 INI 配置文件：" + normalizedPath.wstring();
            return result;
        }

        const std::vector<char> cipherText{
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
        if (input.bad())
        {
            result.error = L"读取加密 INI 配置文件失败：" + normalizedPath.wstring();
            return result;
        }

        std::vector<char> plainText;
        if (!DecryptLegacyWpfConfig(cipherText, plainText, result.error))
        {
            return result;
        }

        std::wstring decodedText;
        bool usedAnsiFallback = false;
        if (!DecodeIniBytes(plainText, decodedText, usedAnsiFallback, result.error))
        {
            SecureZeroMemory(plainText.data(), plainText.size());
            return result;
        }

        std::shared_ptr<ModuleConfig> config(new ModuleConfig());
        config->sourcePath_ = normalizedPath;
        config->document_ = ParseIniText(decodedText);
        config->usedAnsiFallback_ = usedAnsiFallback;
        SecureZeroMemory(plainText.data(), plainText.size());
        std::fill(decodedText.begin(), decodedText.end(), L'\0');

        result.config = std::move(config);
        result.success = true;
        return result;
    }
    catch (const std::exception& exception)
    {
        result.error = L"读取加密 INI 配置文件时发生异常：" +
            ExceptionText(exception.what());
        return result;
    }
    catch (...)
    {
        result.error = L"读取加密 INI 配置文件时发生未知异常。";
        return result;
    }
}

ModuleConfigLoadResult InitializeGlobalModuleConfig(
    const std::filesystem::path& path)
{
    ModuleConfigLoadResult result;
    try
    {
        GlobalConfigState& state = GetGlobalConfigState();
        const std::filesystem::path normalizedPath = path.lexically_normal();
        std::scoped_lock lock(state.mutex);
        if (state.initialized)
        {
            if (PathsEqual(state.sourcePath, normalizedPath))
            {
                return state.result;
            }

            result.error = L"全局配置已经从另一条路径初始化，不能在运行期间重新加载。";
            return result;
        }

        state.initialized = true;
        state.sourcePath = normalizedPath;
        state.result = LoadEncryptedModuleConfig(normalizedPath);
        return state.result;
    }
    catch (const std::exception& exception)
    {
        result.error = L"初始化全局配置时发生异常：" +
            ExceptionText(exception.what());
        return result;
    }
    catch (...)
    {
        result.error = L"初始化全局配置时发生未知异常。";
        return result;
    }
}

std::shared_ptr<const ModuleConfig> GetGlobalModuleConfig()
{
    GlobalConfigState& state = GetGlobalConfigState();
    std::scoped_lock lock(state.mutex);
    return state.result.config;
}

std::wstring GetGlobalModuleConfigError()
{
    GlobalConfigState& state = GetGlobalConfigState();
    std::scoped_lock lock(state.mutex);
    if (!state.initialized)
    {
        return L"全局配置尚未初始化。";
    }
    return state.result.error;
}
} // namespace ftc::modules
