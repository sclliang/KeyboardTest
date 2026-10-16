#pragma once

#include <windows.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ftc::modules
{
enum class LogLevel
{
    Info,
    Warning,
    Error,
};

struct LogMessage final
{
    LogLevel level{LogLevel::Info};
    std::wstring message;
};

struct ProgressUpdate final
{
    std::optional<unsigned int> percent;
    std::wstring text;
};

enum class TestResult
{
    Pass,
    Fail,
    Cancel,
};

struct Completion final
{
    TestResult result{TestResult::Cancel};
    std::wstring message;
};

struct ModuleCallbacks final
{
    std::function<void(LogMessage)> onLog;
    std::function<void(ProgressUpdate)> onProgress;
    std::function<void(Completion)> onCompletion;
};

enum class StartStatus
{
    Started,
    Failed,
};

struct StartOutcome final
{
    StartStatus status{StartStatus::Failed};
    std::wstring message;

    [[nodiscard]] static StartOutcome Success()
    {
        return StartOutcome{StartStatus::Started, {}};
    }

    [[nodiscard]] static StartOutcome Failure(std::wstring reason)
    {
        return StartOutcome{StartStatus::Failed, std::move(reason)};
    }

    [[nodiscard]] bool IsStarted() const noexcept
    {
        return status == StartStatus::Started;
    }
};

class ITestModule
{
public:
    ITestModule() = default;
    virtual ~ITestModule() = default;

    ITestModule(const ITestModule&) = delete;
    ITestModule& operator=(const ITestModule&) = delete;
    ITestModule(ITestModule&&) = delete;
    ITestModule& operator=(ITestModule&&) = delete;

    // A successful start owns its lifecycle until exactly one completion is
    // reported. Before invoking onCompletion, the module must release its
    // windows, child processes, handles, and every worker other than the
    // thread making that final callback. That reporting thread must keep its
    // ModuleCommunication handle by value, must not access module members
    // after reporting the terminal result, and must return immediately. The
    // module destructor must wait for that thread before releasing the object.
    // The host wake callback is asynchronous and must never drain or destroy
    // the module inline on the reporting thread.
    [[nodiscard]] virtual StartOutcome Start(HWND owner, ModuleCallbacks callbacks) = 0;

    // Implementations must make this idempotent and non-blocking. Cleanup and
    // the Cancel completion happen asynchronously inside the module.
    virtual void RequestStop() noexcept = 0;

    // Called on the host UI thread after terminal normalization, before the
    // instance is destroyed or another item starts. Only finalize in-memory
    // records here: no hardware, window work, callbacks, or worker waits.
    // A false return changes the result to FAIL and triggers a second call
    // with Fail to roll back any partial finalization. Non-PASS must therefore
    // be idempotent. A reporting worker must return without waiting for this
    // acknowledgement. Modules without deferred records need no override.
    [[nodiscard]] virtual bool AcceptFinalResult(TestResult) noexcept { return true; }
};

using ModuleFactory = std::function<std::unique_ptr<ITestModule>()>;

struct ModuleDescriptor final
{
    std::wstring key;
    std::wstring defaultChineseName;
    std::wstring defaultEnglishName;
    ModuleFactory factory;
    // Kept after the factory so existing four-field descriptors remain source
    // compatible while modules migrate to the three-language host contract.
    std::wstring defaultVietnameseName;
    // Optional opaque host scheduling key. The host only prevents two running
    // modules from holding the same non-empty group; it does not interpret the
    // key or contain any corresponding test business logic.
    std::wstring exclusiveGroup;
};
} // namespace ftc::modules
