#Requires -Version 5.1
[CmdletBinding()]
param()
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$package = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\package')).Path
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class EditorPackageSmoke {
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window,uint message,IntPtr w,IntPtr l);
}
'@
foreach ($loadLayout in @($false,$true)) {
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = Join-Path $package 'KBeditor.exe'
    $startInfo.WorkingDirectory = $env:TEMP
    $startInfo.UseShellExecute = $false
    $startInfo.EnvironmentVariables['PATH'] = "$env:WINDIR\System32;$env:WINDIR"
    $startInfo.Arguments = if ($loadLayout) { '"' + (Join-Path $package 'Main.KBL') + '" --language en-US' } else { '--language en-US' }
    $expected = if ($loadLayout) { 'Main.KBL' } else { 'Blank layout' }
    $process = [Diagnostics.Process]::Start($startInfo)
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            $process.Refresh()
            if ($process.HasExited) { throw '编辑器提前退出。' }
            if ($process.MainWindowTitle.Contains($expected)) { break }
            Start-Sleep -Milliseconds 50
        } while ([DateTime]::UtcNow -lt $deadline)
        if (-not $process.MainWindowTitle.Contains($expected)) { throw "编辑器未显示预期布局：$expected" }
        Write-Host "正式编辑器启动/加载：$($process.MainWindowTitle)"
        [EditorPackageSmoke]::PostMessage($process.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
        if (-not $process.WaitForExit(5000) -or $process.ExitCode -ne 0) { throw '编辑器正常关闭失败。' }
    } finally {
        if (-not $process.HasExited) {
            $process.Refresh()
            [EditorPackageSmoke]::PostMessage($process.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
            $process.WaitForExit(5000) | Out-Null
        }
        $process.Dispose()
    }
}
Write-Host '正式编辑器无开发环境 PATH 启动、加载及退出检查完成。'
