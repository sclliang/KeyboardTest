#Requires -Version 5.1
[CmdletBinding()]
param([string]$FluentUISdkDirectory, [switch]$RunTests)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath($PSScriptRoot)
$buildRoot = Join-Path $sourceRoot 'build\x64'
$packageRoot = Join-Path $sourceRoot 'package'
$versionMatch = [regex]::Match([IO.File]::ReadAllText((Join-Path $sourceRoot 'CMakeLists.txt')), 'project\(KeyboardTest VERSION (\d+\.\d+\.\d+\.\d+)')
if (-not $versionMatch.Success) { throw '找不到项目版本号。' }
$expectedVersion = $versionMatch.Groups[1].Value
$history = [IO.File]::ReadAllText((Join-Path $sourceRoot 'docs\KeyboardTestHistory.txt'))
if (-not $history.StartsWith("[$expectedVersion]")) { throw '请先更新版本历史。' }
$configure = @('-S', $sourceRoot, '-B', $buildRoot, '-G', 'Visual Studio 18 2026', '-A', 'x64')
if ($FluentUISdkDirectory) { $configure += "-DFTC_FLUENTUI_SDK_DIR=$FluentUISdkDirectory" }
& cmake.exe @configure
if ($LASTEXITCODE -ne 0) { throw 'CMake 配置失败。' }
& cmake.exe --build $buildRoot --config Release --target KeyboardTest KBeditor --parallel
if ($LASTEXITCODE -ne 0) { throw '正式 EXE 编译失败。' }
if ($RunTests) {
    & cmake.exe --build $buildRoot --config Release --target KeyboardTestTests KeyboardApplicationTests KBeditorFormatTests --parallel
    if ($LASTEXITCODE -ne 0) { throw '定向验证程序编译失败。' }
    & ctest.exe --test-dir $buildRoot -C Release --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw '键盘定向验证失败。' }
}
$built = Join-Path $buildRoot 'Release\KeyboardTest.exe'
$info = (Get-Item -LiteralPath $built).VersionInfo
if ($info.FileVersion -ne $expectedVersion -or $info.ProductVersion -ne $expectedVersion -or
    $info.CompanyName -cne 'sixunited' -or $info.ProductName -cne 'KeyboardTest' -or
    $info.OriginalFilename -cne 'KeyboardTest.exe' -or $info.InternalName -cne 'KeyboardTest' -or
    $info.FileDescription -cne 'Standalone Keyboard Test Tool' -or
    $info.LegalCopyright -cne 'Copyright (c) 2026 sixunited. Author: sclliang') {
    throw 'EXE 属性不符合项目约定。'
}
[IO.Directory]::CreateDirectory($packageRoot) | Out-Null
$builtEditor = Join-Path $buildRoot 'src\keyboard_editor\Release\KBeditor.exe'
$editorInfo = (Get-Item -LiteralPath $builtEditor).VersionInfo
if ($editorInfo.FileVersion -ne $expectedVersion -or $editorInfo.ProductVersion -ne $expectedVersion -or
    $editorInfo.CompanyName -cne 'sixunited' -or $editorInfo.ProductName -cne 'KBeditor' -or
    $editorInfo.OriginalFilename -cne 'KBeditor.exe' -or $editorInfo.InternalName -cne 'KBeditor' -or
    $editorInfo.FileDescription -cne 'Keyboard Layout Editor' -or
    $editorInfo.LegalCopyright -cne 'Copyright (c) 2026 sixunited. Author: sclliang') {
    throw '编辑器 EXE 属性不符合项目约定。'
}
Copy-Item -LiteralPath $built -Destination (Join-Path $packageRoot 'KeyboardTest.exe')
Copy-Item -LiteralPath $builtEditor -Destination (Join-Path $packageRoot 'KBeditor.exe')
Copy-Item -LiteralPath (Join-Path $sourceRoot 'docs\KeyboardTestHistory.txt') -Destination $packageRoot
Copy-Item -LiteralPath (Join-Path $sourceRoot 'README.md') -Destination (Join-Path $packageRoot '使用说明.txt')
# Never overwrite the operator's selected layout or language settings.
foreach ($file in @('Main.KBL','config.ini')) {
    $destination = Join-Path $packageRoot $file
    if (-not (Test-Path -LiteralPath $destination)) {
        Copy-Item -LiteralPath (Join-Path $sourceRoot $file) -Destination $destination
    }
}
Write-Host "正式程序：$(Join-Path $packageRoot 'KeyboardTest.exe')"
Write-Host "布局编辑器：$(Join-Path $packageRoot 'KBeditor.exe')"
Write-Host "版本：$expectedVersion；运行时在启动工作目录追加 KeyBoard.log。"
