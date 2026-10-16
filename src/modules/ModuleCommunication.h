#pragma once

#include "modules/ModuleContract.h"

#include <memory>
#include <optional>
#include <string>

namespace ftc::modules
{
// A lightweight, copyable handle for one Start() invocation's callbacks.
// Worker threads should capture this handle by value. All copies share the
// same callbacks and exactly-once terminal gate, so the communication state
// stays alive even while the owning module is being destroyed.
class ModuleCommunication final
{
public:
    explicit ModuleCommunication(ModuleCallbacks callbacks);

    ModuleCommunication(const ModuleCommunication&) noexcept = default;
    ModuleCommunication& operator=(const ModuleCommunication&) noexcept = default;
    ModuleCommunication(ModuleCommunication&&) noexcept = default;
    ModuleCommunication& operator=(ModuleCommunication&&) noexcept = default;

    // A true return value means that the event was accepted while the session
    // was active. Missing callbacks and callback exceptions are isolated from
    // the module and do not change that result.
    // Print is the primary API: the module provides only a level and message;
    // the host callback decides how and where the entry is displayed/stored.
    bool Print(LogLevel level, std::wstring message) const noexcept;
    bool LogInfo(std::wstring message) const noexcept;
    bool LogWarning(std::wstring message) const noexcept;
    bool LogError(std::wstring message) const noexcept;

    bool SetProgress(
        std::optional<unsigned int> percent,
        std::wstring text = {}) const noexcept;
    bool SetProgress(unsigned int percent, std::wstring text = {}) const noexcept;
    bool SetProgressText(std::wstring text) const noexcept;

    // The first terminal report wins across all threads. Later terminal,
    // progress, and log reports are ignored and return false.
    bool Pass(std::wstring message = {}) const noexcept;
    bool Fail(std::wstring message = {}) const noexcept;
    bool Cancel(std::wstring message = {}) const noexcept;

    [[nodiscard]] bool HasCompleted() const noexcept;
    [[nodiscard]] std::optional<TestResult> FinalResult() const noexcept;

private:
    struct State;

    bool Complete(TestResult result, std::wstring message) const noexcept;

    std::shared_ptr<State> state_;
};
} // namespace ftc::modules
