# 星猫键道 Windows 原生版：构建与测试交接

## 当前交付

本轮版本为 **0.1.1+2**，完成 M0-M2 的源码与开发交付流程：独立的 x64/x86 原生 C++17 TSF TIP、x64 librime Host、组合文本、候选 UI、隔离数据目录、依赖校验、双架构构建打包、安装注册及恢复脚本。使用自己的 CLSID/profile，不依赖 Weasel。设置伴侣、正式安装器、语音/AI、词库同步属于 M3-M5。

当前开发机器是 macOS。协议测试、依赖准备和脚本替身检查已经执行；真实 Windows/MSVC 编译、COM/TSF 注册和输入体验尚未验收。本目录提供的是待 Windows 构建和实测的源码，不能将其理解为已经运行验证的 Windows 二进制。

Host/librime 固定 x64；x86 TIP 连接同一个 x64 Host。构建和输入使用普通桌面会话；安装、系统注册和注销使用**管理员的 64 位 PowerShell**。Host 拒绝提权运行。脚本不更改默认输入法，不删除用户词库。

## Windows 前置条件

- Windows 11 x64，64 位 Windows PowerShell 5.1 或更新的 PowerShell。
- Visual Studio 2022：Desktop development with C++、MSVC v143、Windows 10/11 SDK，包含 x64/x86 工具。
- CMake 3.24 或更新版本；同目录应有 CTest。
- `7z.exe` 在 PATH 中；首次下载依赖需要网络。
- 完整仓库，包括 `xmjd/xmjd.zip`；也可使用本轮源码交接包 `XMJD-0.1.1+2-windows-source.zip`，解压后保留 `Windows/` 和 `xmjd/xmjd.zip` 的相对位置。不要仅复制 `Windows/` 源码目录。

以下命令使用系统自带的 `powershell.exe`；也可使用 64 位 `pwsh`。示例的 `-ExecutionPolicy Bypass` 仅作用于该命令进程，不修改系统执行策略；请先核对源码来源。不要从 32 位 PowerShell 执行安装注册。

## 1. 普通 PowerShell 构建

在仓库根目录执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\build.ps1
```

默认 `-Architecture all -Configuration Release`。脚本依次准备和复核锁定依赖，以 Visual Studio 2022 生成器构建 x64 和 Win32，分别运行 CTest，合并安装到 staging，生成逐文件 SHA-256 manifest 和 ZIP，再校验交付目录和 ZIP。任何原生命令失败都会停止。默认不安装到系统目录、不注册。

输出为：

```text
Windows/out/build/x64/Release/
  XMJDHost.exe、运行时 DLL、Resources/Rime、Tip/x64/XMJDTip.dll
  ProtocolTests.exe、XMJDTipComTests.exe、EditorHarness.exe
Windows/out/build/x86/Release/
  Tip/x86/XMJDTip.dll
  ProtocolTests.exe、XMJDTipComTests.exe、EditorHarness.exe
Windows/out/package/
  XMJD-0.1.1+2-windows-x64-x86-Release/
  XMJD-0.1.1+2-windows-x64-x86-Release.zip
```

交付目录含两个 TIP、x64 Host、全部依赖 DLL、Rime/OpenCC 数据、锁文件、版本信息、第三方许可证、文档和安装注册脚本。测试工具保留在对应构建目录。已有同名交付不会覆盖；要重新打包，先将先前交付另存，再重新运行。

可单独构建某架构，或使用 Debug；单架构不会生成合并交付：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\build.ps1 -Architecture x64 -Configuration Debug
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\build.ps1 -Architecture x86 -Configuration Debug
```

已有依赖仍需逐文件校验，不能仅信标记文件：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\prepare-deps.ps1 -Architecture x64 -VerifyOnly
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\build.ps1 -VerifyPackage ".\Windows\out\package\XMJD-0.1.1+2-windows-x64-x86-Release.zip"
```

`-VerifyPackage` 同时支持目录和 ZIP，可在 macOS PowerShell 上运行。它检查布局、链接、重复/越界路径、文件大小和内容哈希；manifest 是完整性清单，不代替发布者签名或可信来源验证。

## 2. 管理员 PowerShell 安装并注册

构建成功后，打开**管理员的 64 位 PowerShell**，进入同一仓库根目录：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\install.ps1
$installDirectory = "$env:ProgramFiles\XMJD\0.1.1+2"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\register.ps1 -InstallDirectory $installDirectory
```

安装先验证完整交付，复制到 Program Files 下的临时目录，复核后发布到 `%ProgramFiles%\XMJD\0.1.1+2`。同版本安装目录不覆盖。可以通过 `-PackageDirectory` 选择已验证的解压交付；交付内的 `scripts/install.ps1` 默认安装其所在交付。Debug 交付需要 `-Configuration Debug`。

注册固定同时处理 x64/x86：分别通过 System32/SysWOW64 的 `regsvr32.exe` 注册对应 TIP。COM 位于 **HKLM 的对应 64/32 位视图**；两种 TIP 均读取 **HKLM 64 位视图 `Software\XMJD\HostExecutablePath`**。完成标记为 `RegistrationComplete`；HKLM 的 `Software\Microsoft\Windows\CurrentVersion\Run\XMJDHost` 保存带完整引号的 Host 路径，供普通用户下次登录启动。安装注册脚本不启动提权 Host。

全部前置检查通过后才调用注册。相同安装的完整重复注册不再执行；部分已有注册要求先对该安装执行注销再重试。失败时逆序回滚本次尝试；清理失败保留所有权元数据供重试。不允许覆盖指向其他目录的 XMJD 注册。

## 3. 普通桌面会话启用并测试

回到**非管理员**桌面会话，在 Windows 设置的中文语言/键盘选项中手动添加或选择 **XMJD**，使用 Win+Space 切换；具体设置界面随 Windows 版本变化。

TIP 激活时先连接已运行 Host，必要时异步启动，500 ms 重试；进程启动按进程限频 2 秒，不等待启动、不重放按键。首次启动需要部署方案，此时输入暂不可用。也可在普通 PowerShell 中预先执行部署（必须没有同账户的 Host 正在运行）：

```powershell
$installDirectory = "$env:ProgramFiles\XMJD\0.1.1+2"
& "$installDirectory\XMJDHost.exe" --deploy
```

正常结果为 `deploy-ok`，然后切换到 XMJD 触发 Host 启动，或执行：

```powershell
Start-Process -FilePath "$installDirectory\XMJDHost.exe" -WindowStyle Hidden
& "$installDirectory\XMJDHost.exe" --health
```

`--health` 只查询，不启动 Host；初始化尚未完成或没有运行时返回 `unavailable`。健康时返回 `ok`。Host 初始化错误会在普通终端输出状态和错误码；不要通过管理员运行绕过错误。

所有可写数据位于 `%LOCALAPPDATA%\XMJD`：`SharedData\Rime`、`UserData\Rime`、`Staging`、`Backups`、`Logs`。不会使用 `%APPDATA%\Rime`。首次发布带 journal/pending 状态；完整资源发布后若 runtime 初始化失败，重启继续部署；缺少必需 Lua/OpenCC 数据不会标记 ready。`--deploy` 会对当前独立资源执行维护；本轮不提供已有资源的导入/版本替换，资源升级界面与迁移属于 M3，不会在键入时悄悄升级。

## Windows 验收清单

记录 Windows build、CPU、MSVC/SDK/CMake 版本、命令输出和失败复现步骤。建议先使用干净的 Windows 11 x64 测试账户或可丢弃虚拟机。

- 默认双架构构建及两个架构的全部 CTest 通过。COM 测试只加载 DLL、不注册；EditorHarness smoke 仅验证控件创建与读写，不能替代输入测试。
- 分别运行 `Windows/out/build/x64/Release/EditorHarness.exe` 和 x86 版本，在 Edit/RichEdit 两字段测试组合、候选、鼠标选词、方案分页、Esc、中文标点、英文模式和 Ctrl 快捷键；特别检查左右 Shift 的 `commit_code` 只提交一次、Ctrl+Shift 不误触发单独 Shift、Caps Lock、F4/Ctrl+grave 方案选单。Ctrl+Tab 切换字段。
- 继续测试 Notepad、Edge/Chrome、VS Code/Electron、常用聊天编辑器；Office 仅在实际安装时记录。
- 中文与非 BMP 字符、候选注释、光标移动、100/150/200% DPI、混合显示器和负坐标；候选窗口不抢焦点，应用接管候选 UI 时无重复窗口。
- 两窗口/两字段/两应用隔离；旧候选点击、切换输入法、焦点变化、应用关闭和重复激活不能把文本写入新字段。
- Host 未启动、首次部署、停止/崩溃后恢复、处理后回复超时：应用保持响应，不重放、不重复提交；只在原存活上下文使用最后已确认的原始编码恢复。
- 首帧不发送超过 5 秒的连接会释放；收到首字节后半帧必须在 50 ms 内完成。验证非法长度/UTF-8、身份不匹配、32 连接、CancelIoEx 和关闭恢复。真实管道行为尚待 Windows 测试。
- 锁定依赖/资源篡改、缺失 Lua/OpenCC、数据根被占用、部署中断和磁盘失败明确报错；保留学习数据，不用删除词库作为修复手段。
- 注册、完整重复注册、部分失败回滚、注销两次、不同安装路径拒绝、注销后重新注册；检查两个 COM 视图和 HKLM 元数据。
- 记录热态按键延迟，目标 p95 <=10 ms，输入事务上限 50 ms；这些是待测目标，尚无 Windows 性能结果。

初始支持范围为普通完整信任桌面应用。管理员、AppContainer/受限宿主、远程桌面、游戏和反作弊环境没有通过兼容性验收。

## 注销与恢复

先切换到其他输入法，关闭测试编辑器；在管理员的 64 位 PowerShell 中执行：

```powershell
$installDirectory = "$env:ProgramFiles\XMJD\0.1.1+2"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\unregister.ps1 -InstallDirectory $installDirectory
```

也可使用安装目录下的 `scripts/unregister.ps1`。注销在修改前检查两个架构与路径所有权；部分失败保留元数据并报告，修正后用同一目录重试。成功后清理匹配安装的 COM/TSF、Host 路径、完成标记和启动项，保留全部用户数据。注销不强杀应用、不删除程序目录；注销会话后再处理已加载的旧版本文件。

Host 启动错误常见输出：`identity-error`（错误权限/会话）、`already-running`、`data-root-busy`、`runtime-error <code>`、`pipe-error`。若部署失败，保留输出和 `%LOCALAPPDATA%\XMJD` 中的恢复状态。不要改动 Weasel 或其他 Rime 目录。

## 依赖与本机验证记录

`librime.lock.json` 固定官方 librime **1.16.1**、core commit `de4700e9f6b75b109910613df907965e3cbe0567`、内置 librime-lua commit `68f9c364a2d25a04c7d4794981d7c796b05ab627`、**Lua 5.4.8**、**OpenCC 1.1.9**，以及依赖、方案和许可证 SHA-256。Lua 可静态集成在 `rime.dll`；Host 仍必须通过 `find_module("lua")`，缺模块时 fail closed。

官方工件同时包含 octagram（GPL-3.0）。交付保留第三方许可原文与来源；公开分发前仍需落实对应许可要求、签名和正式安装器。本轮未公开发布。

在 macOS 已完成：portable CMake/CTest **1/1**；C++17 严格编译与 ASan/UBSan；官方依赖准备与重复校验（184 文件、30 个 OpenCC 文件、15 个许可文件）；危险归档、内容篡改和包清单反例；资源合并/安装规则；五个 PowerShell 脚本语法解析；注册的首次/重复/逆序回滚/清理失败重试/所有权检查替身；Host 的单次 commit、失败恢复与缺资源拒绝桩检查。从实际 TIP 源码提取的按键方法，也已对 SDK 虚拟键常量及模拟键盘状态完成检查。COM 测试补充 thread/profile sink 分阶段注册失败清理，等待 Windows 执行。替身结果不构成 Windows SDK 编译、真实 PowerShell 5.1、TSF/COM 或真实 librime 初始化通过的证据。

可在 macOS 重跑可移植目标：

```sh
cmake -S Windows -B Windows/out/portable -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build Windows/out/portable --parallel
ctest --test-dir Windows/out/portable --output-on-failure --no-tests=error
```

`Windows/VERSION` 是 Windows 唯一版本源；本轮保持 0.1.1+2。依赖、构建产物和 staging 不提交到仓库。
