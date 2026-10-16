#include "modules/ModuleCommunication.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace ftc::modules
{
struct ModuleCommunication::State final
{
    enum class TerminalState : unsigned char
    {
        Active,
        Passed,
        Failed,
        Cancelled,
    };

    explicit State(ModuleCallbacks value) noexcept
        : callbacks(std::move(value))
    {
    }

    std::mutex dispatchMutex;
    ModuleCallbacks callbacks;
    std::atomic<TerminalState> terminalState{TerminalState::Active};
};

ModuleCommunication::ModuleCommunication(ModuleCallbacks callbacks)
    : state_(std::make_shared<State>(std::move(callbacks)))
{
}

bool ModuleCommunication::LogInfo(std::wstring message) const noexcept
{
    return Print(LogLevel::Info, std::move(message));
}

bool ModuleCommunication::LogWarning(std::wstring message) const noexcept
{
    return Print(LogLevel::Warning, std::move(message));
}

bool ModuleCommunication::LogError(std::wstring message) const noexcept
{
    return Print(LogLevel::Error, std::move(message));
}

bool ModuleCommunication::SetProgress(
    const std::optional<unsigned int> percent,
    std::wstring text) const noexcept
{
    const std::shared_ptr<State> state = state_;
    if (!state)
    {
        return false;
    }
    try
    {
        const std::scoped_lock lock(state->dispatchMutex);
        if (state->terminalState.load(std::memory_order_acquire) != State::TerminalState::Active)
        {
            return false;
        }

        if (state->callbacks.onProgress)
        {
            try
            {
                state->callbacks.onProgress(ProgressUpdate{percent, std::move(text)});
            }
            catch (...)
            {
                // A host callback must never unwind into module code.
            }
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ModuleCommunication::SetProgress(
    const unsigned int percent,
    std::wstring text) const noexcept
{
    return SetProgress(std::optional<unsigned int>{percent}, std::move(text));
}

bool ModuleCommunication::SetProgressText(std::wstring text) const noexcept
{
    return SetProgress(std::nullopt, std::move(text));
}

bool ModuleCommunication::Pass(std::wstring message) const noexcept
{
    return Complete(TestResult::Pass, std::move(message));
}

bool ModuleCommunication::Fail(std::wstring message) const noexcept
{
    return Complete(TestResult::Fail, std::move(message));
}

bool ModuleCommunication::Cancel(std::wstring message) const noexcept
{
    return Complete(TestResult::Cancel, std::move(message));
}

bool ModuleCommunication::HasCompleted() const noexcept
{
    const std::shared_ptr<State> state = state_;
    return state &&
        state->terminalState.load(std::memory_order_acquire) != State::TerminalState::Active;
}

std::optional<TestResult> ModuleCommunication::FinalResult() const noexcept
{
    const std::shared_ptr<State> state = state_;
    if (!state)
    {
        return std::nullopt;
    }
    switch (state->terminalState.load(std::memory_order_acquire))
    {
    case State::TerminalState::Active:
        return std::nullopt;
    case State::TerminalState::Passed:
        return TestResult::Pass;
    case State::TerminalState::Failed:
        return TestResult::Fail;
    case State::TerminalState::Cancelled:
        return TestResult::Cancel;
    }
    return std::nullopt;
}

bool ModuleCommunication::Print(
    const LogLevel level,
    std::wstring message) const noexcept
{
    const std::shared_ptr<State> state = state_;
    if (!state)
    {
        return false;
    }
    try
    {
        const std::scoped_lock lock(state->dispatchMutex);
        if (state->terminalState.load(std::memory_order_acquire) != State::TerminalState::Active)
        {
            return false;
        }

        if (state->callbacks.onLog)
        {
            try
            {
                state->callbacks.onLog(LogMessage{level, std::move(message)});
            }
            catch (...)
            {
                // A host callback must never unwind into module code.
            }
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool ModuleCommunication::Complete(
    const TestResult result,
    std::wstring message) const noexcept
{
    const std::shared_ptr<State> state = state_;
    if (!state)
    {
        return false;
    }
    try
    {
        const std::scoped_lock lock(state->dispatchMutex);
        if (state->terminalState.load(std::memory_order_acquire) != State::TerminalState::Active)
        {
            return false;
        }

        State::TerminalState terminalState = State::TerminalState::Cancelled;
        switch (result)
        {
        case TestResult::Pass:
            terminalState = State::TerminalState::Passed;
            break;
        case TestResult::Fail:
            terminalState = State::TerminalState::Failed;
            break;
        case TestResult::Cancel:
            terminalState = State::TerminalState::Cancelled;
            break;
        }
        state->terminalState.store(terminalState, std::memory_order_release);
        if (state->callbacks.onCompletion)
        {
            try
            {
                state->callbacks.onCompletion(Completion{result, std::move(message)});
            }
            catch (...)
            {
                // The first terminal report remains final even if the callback throws.
            }
        }
        return true;
    }
    catch (...)
    {
        return false;
    }
}
} // namespace ftc::modules
