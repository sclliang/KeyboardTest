#include "KeyboardLogic.h"
#include <algorithm>

namespace ftc::modules::keyboard_test
{
KeyboardLogic::KeyboardLogic(KeyboardLayout layout) : layout_(std::move(layout)) { Reset(); }
void KeyboardLogic::Reset()
{
    status_.assign(layout_.keys.size(), KeyStatus::Waiting);
    armed_.assign(layout_.keys.size(), false);
    chords_.assign(layout_.keys.size(), {});
    presses_.clear(); previous_ = static_cast<std::size_t>(-1); previousTime_ = 0;
}
void KeyboardLogic::Suspend()
{
    for (auto& status : status_) if (status == KeyStatus::Pressed) status = KeyStatus::Waiting;
    std::fill(armed_.begin(), armed_.end(), false);
    chords_.assign(layout_.keys.size(), {});
    presses_.clear(); previous_ = static_cast<std::size_t>(-1); previousTime_ = 0;
}
bool KeyboardLogic::Matches(const Binding& expected, const Binding& actual) noexcept
{
    return expected.scan ? expected.scan == actual.scan && expected.extended == actual.extended
        : expected.vk != 0 && expected.vk == actual.vk;
}
bool KeyboardLogic::Down(const Binding& binding) const noexcept
{
    return std::any_of(presses_.begin(), presses_.end(), [&](const Press& press) {
        return press.down && Matches(binding, press.binding);
    });
}
void KeyboardLogic::Feed(const KeyEvent& event)
{
    auto found = std::find_if(presses_.begin(), presses_.end(), [&](const Press& press) {
        return Matches(press.binding, event.binding);
    });
    if (event.down)
    {
        if (found != presses_.end() && found->down) return; // Auto-repeat.
        if (found == presses_.end())
        {
            if (presses_.size() >= 512) { Suspend(); return; }
            presses_.push_back({event.binding, true, false}); found = presses_.end() - 1;
        }
        else { found->down = true; found->consumed = false; }
    }
    else
    {
        if (found == presses_.end() || !found->down) return; // Orphan release / pre-held key.
        found->down = false;
    }
    // Recognize complete combinations before updating any single-key result.
    for (std::size_t i = 0; i < layout_.keys.size(); ++i)
    {
        const auto& bindings = layout_.keys[i].bindings;
        if (bindings.size() < 2 || chords_[i].armed || !event.down) continue;
        if (std::all_of(bindings.begin(), bindings.end(), [&](const Binding& b) { return Down(b); }))
        {
            chords_[i].armed = true; chords_[i].pending = bindings;
            status_[i] = KeyStatus::Pressed;
            for (auto& press : presses_)
                if (press.down && std::any_of(bindings.begin(), bindings.end(), [&](const Binding& b) {
                    return Matches(b, press.binding);
                })) press.consumed = true;
        }
    }
    for (std::size_t i = 0; i < layout_.keys.size(); ++i)
    {
        const auto& bindings = layout_.keys[i].bindings;
        if (bindings.size() != 1 || !Matches(bindings.front(), event.binding)) continue;
        if (event.down)
        {
            armed_[i] = true;
            if (status_[i] != KeyStatus::Passed) status_[i] = KeyStatus::Pressed;
            // Composite members are not subject to the legacy 20 ms inter-key rule.
            const bool member = std::any_of(layout_.keys.begin(), layout_.keys.end(), [&](const auto& key) {
                return key.bindings.size() > 1 && std::any_of(key.bindings.begin(), key.bindings.end(),
                    [&](const Binding& b) { return Matches(b, event.binding); });
            });
            if (!member && layout_.stickCheck && previous_ != static_cast<std::size_t>(-1) &&
                previous_ != i && event.time >= previousTime_ && event.time - previousTime_ < 20)
            {
                status_[i] = KeyStatus::Retry; armed_[i] = false;
                if (status_[previous_] != KeyStatus::Passed)
                { status_[previous_] = KeyStatus::Retry; armed_[previous_] = false; }
            }
            if (!member) { previous_ = i; previousTime_ = event.time; }
            if (event.binding.vk == VK_PAUSE && armed_[i] && !found->consumed)
            { status_[i] = KeyStatus::Passed; armed_[i] = false; found->down = false; }
        }
        else
        {
            if (armed_[i] && !found->consumed) status_[i] = KeyStatus::Passed;
            else if (status_[i] == KeyStatus::Pressed) status_[i] = KeyStatus::Waiting;
            armed_[i] = false;
        }
    }
    if (!event.down)
    {
        for (std::size_t i = 0; i < chords_.size(); ++i)
        {
            auto& chord = chords_[i];
            if (!chord.armed) continue;
            std::erase_if(chord.pending, [&](const Binding& b) { return Matches(b, event.binding); });
            if (chord.pending.empty()) { status_[i] = KeyStatus::Passed; chord.armed = false; }
        }
    }
}
unsigned KeyboardLogic::Passed() const noexcept
{
    return static_cast<unsigned>(std::count(status_.begin(), status_.end(), KeyStatus::Passed));
}
bool KeyboardLogic::Complete() const noexcept { return Passed() == layout_.keys.size() && !layout_.keys.empty(); }
}
