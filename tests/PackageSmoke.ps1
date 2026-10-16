#Requires -Version 5.1
[CmdletBinding()]
param([string]$Executable = (Join-Path $PSScriptRoot '..\package\KeyboardTest.exe'))
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$fixtureRoot = Join-Path $PSScriptRoot ('..\artifacts\package-smoke-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixtureRoot) | Out-Null
$fixtureRoot = (Resolve-Path -LiteralPath $fixtureRoot).Path
$workingDirectory = Join-Path $fixtureRoot '工作 目录'
$layouts = Join-Path $workingDirectory 'Layouts'
[IO.Directory]::CreateDirectory($layouts) | Out-Null
$layoutText = "KBL2][1][300][200][0`r`nF24][10][35][50][50][0][0][Single][0,0,135`r`n"
$utf8 = New-Object Text.UTF8Encoding($false)
[IO.File]::WriteAllText((Join-Path $workingDirectory 'Main.KBL'),$layoutText,$utf8)
[IO.File]::WriteAllText((Join-Path $layouts 'Key Board.KBL'),$layoutText,$utf8)
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class KeyboardPackageSmoke {
    [StructLayout(LayoutKind.Explicit,Size=40)] public struct Input {
        [FieldOffset(0)] public uint Type;
        [FieldOffset(8)] public ushort Vk;
        [FieldOffset(12)] public uint Flags;
    }
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr window,int id);
    [DllImport("user32.dll")] public static extern bool IsWindowEnabled(IntPtr window);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr window,uint message,IntPtr w,IntPtr l);
    [DllImport("user32.dll",SetLastError=true)] static extern uint SendInput(uint count,Input[] inputs,int size);
    public static uint PressF24() {
        return SendInput(2,new[] {new Input {Type=1,Vk=135},new Input {Type=1,Vk=135,Flags=2}},40);
    }
}
'@
$cases = @('', '"Layouts\Key Board.KBL"', ('"' + (Join-Path $layouts 'Key Board.KBL') + '"'))
foreach ($arguments in $cases) {
    $startInfo = New-Object Diagnostics.ProcessStartInfo
    $startInfo.FileName = $executablePath
    $startInfo.Arguments = $arguments
    $startInfo.WorkingDirectory = $workingDirectory
    $startInfo.UseShellExecute = $false
    $startInfo.EnvironmentVariables['PATH'] = "$env:WINDIR\System32;$env:WINDIR"
    $process = [Diagnostics.Process]::Start($startInfo)
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(10)
        $ready = $false
        while ([DateTime]::UtcNow -lt $deadline -and -not $process.HasExited) {
            $process.Refresh()
            $window = $process.MainWindowHandle
            if ($window -ne [IntPtr]::Zero -and [KeyboardPackageSmoke]::IsWindowEnabled([KeyboardPackageSmoke]::GetDlgItem($window,101))) {
                $ready = $true
                break
            }
            Start-Sleep -Milliseconds 50
        }
        if (-not $ready) { throw '正式包未能启动并加载测试布局。' }
        [KeyboardPackageSmoke]::SetForegroundWindow($window) | Out-Null
        Start-Sleep -Milliseconds 200
        if ([KeyboardPackageSmoke]::GetForegroundWindow() -ne $window) { throw '测试窗口不在前台，停止发送输入。' }
        if ([KeyboardPackageSmoke]::PressF24() -ne 2) { throw 'SendInput 未发送完整按下/松开事件。' }
        if (-not $process.WaitForExit(5000)) { throw 'PASS 后未自动退出。' }
        if ($process.ExitCode -ne 0) { throw "正式包退出码错误：$($process.ExitCode)" }
        Write-Host "正式包 PASS，参数：[$arguments]"
    } finally {
        if (-not $process.HasExited) {
            $process.Refresh()
            [KeyboardPackageSmoke]::PostMessage($process.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
            $process.WaitForExit(5000) | Out-Null
        }
        $process.Dispose()
    }
}
$logPath = Join-Path $workingDirectory 'KeyBoard.log'
$log = [IO.File]::ReadAllText($logPath)
if ([regex]::Matches($log,'Result: PASS').Count -ne 3) { throw '缺少三个独立 PASS 记录。' }
if (-not $log.Contains((Join-Path $workingDirectory 'Main.KBL')) -or
    -not $log.Contains((Join-Path $layouts 'Key Board.KBL'))) { throw '日志中的布局路径不正确。' }
Write-Host "正式包无开发环境 PATH 验证完成；模拟输入，不替代实体键盘实测。日志：$logPath"
