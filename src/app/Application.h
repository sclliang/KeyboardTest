#pragma once
#include "modules/ModuleContract.h"
#include <filesystem>

namespace keyboard_app
{
// Tests provide the original module's no-hook fixture. The production EXE
// exposes no simulated-input switch and always installs its real keyboard hook.
std::filesystem::path ResolveLayoutPath(const std::filesystem::path& workingDirectory,
    const std::filesystem::path& argument = L"Main.KBL");
int RunApplication(const std::filesystem::path& workingDirectory, const std::filesystem::path& layoutPath,
    ftc::modules::ModuleFactory factory = {}, bool reportErrors = true);
}
