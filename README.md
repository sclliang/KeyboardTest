# KeyboardTest 独立键盘测试工具

原生 Win32/C++ x64，版本 1.0.10.16，公司 sixunited，作者 sclliang。
直接复制 FactoryTestCases 当前键盘测试模块，保留原有布局、输入和判定逻辑。

公开源码：https://github.com/sclliang/KeyboardTest
正式程序与编辑器：https://github.com/sclliang/KeyboardTest/releases/tag/v1.0.10.16

## 使用

双击 package\KeyboardTest.exe 即开始测试。默认读取**启动工作目录**下的 Main.KBL。
全部配置按键通过后，先把完整记录追加并刷新到**启动工作目录\KeyBoard.log**，随后自动退出，无需点击确认。
“当前目录”指进程的工作目录；命令行中就是执行命令时所在的目录，快捷方式可通过“起始位置”设置。

```powershell
# 在 package 目录运行，读取 Main.KBL
.\KeyboardTest.exe

# 读取当前目录的 KeyBoard.KBL
.\KeyboardTest.exe KeyBoard.KBL

# 相对路径（包括子目录）
.\KeyboardTest.exe .\Layouts\Laptop.KBL

# 绝对路径；包含空格时加双引号
.\KeyboardTest.exe "D:\Keyboard Layouts\Laptop.KBL"
```

即使传入别处的绝对 KBL 路径，日志仍保存在启动工作目录，不跟随 KBL 所在目录。
例如从 D:\Station 执行 D:\Code\KeyboardTest\package\KeyboardTest.exe 时，默认读取
D:\Station\Main.KBL，日志写入 D:\Station\KeyBoard.log。

**随包 Main.KBL 仅为当前源程序的 13 键样例（Esc、F1–F12）。**
只有写入 KBL 的按键参与测试；完整键盘需要替换成目标机器的完整布局。
兼容已有 KBL 和 KBL2，随包 KBeditor.exe 用于制作或修改布局。发布脚本保留已存在的 Main.KBL。

灰色等待，蓝色按下，绿色通过，黄色需要松开后重试。保留组合键、Pause、20 ms 连键检查、失焦暂停和重新测试。
全部配置键通过才 PASS；“结束测试”或关闭窗口为 FAIL；无整体超时。
Ctrl+Alt+Del 等系统保留组合、仅由固件处理的 Fn 不保证可捕获。

## 布局编辑器

双击同目录 KBeditor.exe，从空白布局开始；点击“打开 KBL”选择已有文件，或通过参数打开：

```powershell
.\KBeditor.exe Main.KBL
.\KBeditor.exe "D:\Keyboard Layouts\Laptop.KBL" --language en-US
```

保留原编辑器的连续添加按键、鼠标“下一行”、拖动位置/尺寸、重新捕获、组合键、
撤销重做、保存/另存为和内部预览功能；默认保存位置是编辑器所在目录。
将完整布局保存为测试工作目录的 Main.KBL，或运行 KeyboardTest.exe 时传入该文件。
编辑器与测试程序各自维护完整实现，不依赖原 FactoryTestCases 程序。

## 日志和退出码

KeyBoard.log 为 UTF-8 追加日志，记录程序版本、机器名、进程号、工作目录、KBL 完整路径、
开始/结束时间、耗时、PASS/FAIL/CANCEL、通过数量和必要错误。重新测试后数量只表示最后一轮。
不记录普通逐键输入；以前的测试记录保留。测试结束前日志暂存在内存，异常强杀进程不会生成完整结束记录。

- 0：PASS，日志已保存，程序自动退出。
- 1：FAIL（包括手动关闭、布局缺失或格式无效）。
- 2：CANCEL。
- 3：日志写入失败或程序无法启动。写日志失败会显示文件路径及 Windows 错误码，不会返回成功码。

## 可选语言

无 config.ini 也可运行，默认中文。需要切换时，在工作目录放置明文 UTF-8 config.ini：

```ini
[language]
Culture=zh-CN
```

支持 zh-CN、en-US、vi-VN，名称比较不区分大小写。KBL 只由启动参数选择，
不读取旧 [KeyboardTest] LayoutFile；不需要 IniEditor 或原工厂测试程序。

## 构建

Windows x64、Visual Studio 2026 C++ 工具链、CMake、相邻 FluentUI/sdk 原生静态 SDK。

```powershell
.\build-release.ps1
# 必要的键盘逻辑与独立应用退出/日志定向验证
.\build-release.ps1 -RunTests
# SDK 可显式指定，不在代码内固定个人路径
.\build-release.ps1 -FluentUISdkDirectory D:\Code\FluentUI\sdk
```

输出 package\KeyboardTest.exe 和 package\KBeditor.exe，均静态链接 C++ 运行库和 FluentUI，运行只需 Windows 系统 DLL。
无需 WPF、Qt、VC 运行库安装器或开发机 SDK。Main.KBL 是测试数据，并非 DLL 依赖。
发布包不包含空 bin 目录或其他测试模块。

来源与行为记录见 docs\BEHAVIOR_BASELINE.md；详细验证及未实测范围见 docs\releases\v1.0.10.16.md。
