#pragma once
#include "KeyboardLayout.h"
#include <array>

namespace ftc::keyboard_editor
{
enum class KeyStatus { Waiting, Pressed, Passed, Retry };
struct KeyEvent final { Binding binding; bool down{}; ULONGLONG time{}; };
class KeyboardLogic final
{
public:
    explicit KeyboardLogic(KeyboardLayout layout);
    void Reset();
    void Suspend();
    void Feed(const KeyEvent& event);
    const KeyboardLayout& Layout() const noexcept { return layout_; }
    const std::vector<KeyStatus>& Status() const noexcept { return status_; }
    unsigned Passed() const noexcept;
    bool Complete() const noexcept;
private:
    struct Press final { Binding binding; bool down{}, consumed{}; };
    struct Chord final { bool armed{}; std::vector<Binding> pending; };
    static bool Matches(const Binding&, const Binding&) noexcept;
    bool Down(const Binding&) const noexcept;
    KeyboardLayout layout_;
    std::vector<KeyStatus> status_;
    std::vector<bool> armed_;
    std::vector<Chord> chords_;
    std::vector<Press> presses_;
    std::size_t previous_{static_cast<std::size_t>(-1)};
    ULONGLONG previousTime_{};
};
}
