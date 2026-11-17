# 星猫键道 Windows 原生版

星猫键道独立 Windows 版本，当前版本 **0.1.5+8**。采用原生 C++17 TSF 前端，提供 x64/x86 输入法 DLL，由一个 x64 Host 运行 librime。无需安装 Weasel，也不读取或修改 Weasel 的用户目录。

## 下载并安装

公开仓库：<https://github.com/Attiv/xmjd-windows>。

安装包直接下载（无需登录）：

<https://github.com/Attiv/xmjd-windows/releases/download/v0.1.4%2B5/XMJD-0.1.4%2B5-windows-x64-setup.exe>

SHA-256：`39c23185a801e2878c3bf0986e3e2c2d94aa8198ef478aa6d4fb70d0045772bd`

1. 下载 `XMJD-0.1.5+8-windows-x64-setup.exe`。支持 Windows 11 x64；不支持 ARM64。
2. 运行安装程序并允许管理员安装。程序自动安装并注册 x64/x86 输入法，无需安装开发工具或执行构建命令。
3. 安装结束后会以普通用户身份打开**星猫键道设置**。如果窗口没有出现，从开始菜单打开“星猫键道 → 星猫键道设置”。
4. 在设置里选择输入方案，点击**应用并部署**，等待词典编译完成；需要外部方案或换皮肤，见下面两节。
5. 返回普通桌面会话，在中文语言的键盘选项中添加或选择 **XMJD**，再用 **Win+Space** 切换。安装程序不修改默认输入法。
6. 首次使用要部署方案和词典，请等待初始化。必要时注销后重新登录，再切换到 XMJD。

安装程序目前没有商业代码签名证书。请从本仓库 Release 下载，并按附件中的 `SHA256SUMS.txt` 核对。Windows 可能显示未知发布者提示。

程序目录：`%ProgramFiles%\XMJD\0.1.5+8`。用户数据独立保存在 `%LOCALAPPDATA%\XMJD`，包括 `SharedData`、`UserData`、`Staging`、`Backups`、`Logs`。正常运行使用普通用户权限；Host 拒绝管理员、受限和 AppContainer 会话。

## 设置与方案选择

设置程序 `XMJDSettings.exe` 位于程序目录，开始菜单也有入口。它提供：

- **输入方案**：`星猫键道6`（主方案，形码）、`袖珍简化字拼音`、`LiangFen`。切换后会把所选方案放到列表首位，并重建 `%LOCALAPPDATA%\XMJD\UserData\Rime\default.custom.yaml` 的 `schema_list`。
- **应用并部署**：写入设置并让引擎重新编译词典。首次编译可能需要一分钟；部署期间不要连续按键。
- **重新部署**：方案文件被修改或词典异常时使用。
- **启动引擎**：查看/启动普通用户会话中的 `XMJDHost.exe`。
- **打开用户目录 / 打开日志 / 语言设置**：定位数据、查看诊断日志，或直接打开 Windows 的语言设置页。

运行中的输入法也可以用 **F4** 或 **Ctrl+grave** 呼出方案选单；切换后会在会话销毁时写回 `settings.ini`，新字段继续使用该方案。若刚切换方案，请切换一次窗口或重新输入，让输入法建立新会话。

设置程序必须以普通用户身份运行。管理员窗口只能修改配置文件，不能启动引擎。方案列表来自程序目录 `Resources\Rime\*.schema.yaml` 和用户目录 `%LOCALAPPDATA%\XMJD\UserData\Rime`，标记“勿选”的辅助方案不会显示。

## 方案来源（外部 Rime 方案目录）

设置里的**方案来源**可以把已有的 Rime 方案目录（例如鼠须管目录、别人分享的方案包）接进星猫：

1. 点**选择目录…**，选中根层直接包含 `.schema.yaml` 的目录。
2. 星猫只读取并复制到自己的用户目录 `%LOCALAPPDATA%\XMJD\UserData\Rime\imported\<目录名>\`，不改动来源目录，也不写入它的 build。
3. 复制时跳过 `build`、`*.userdb`、`user.yaml`、`installation.yaml`、`trash` 等运行时文件；只接受方案、词典、Lua、OpenCC 和图片等资源，上限 20,000 个文件 / 512 MiB。
4. 回到方案列表选中新方案，点**应用并部署**。部署时会逐方案验证，个别第三方方案编译失败只会在 `Logs\schema-skipped.log` 里记录，不会让整个引擎不可用。
5. **移除导入方案**会删掉 `imported` 目录，内置方案和用户词库不受影响；如果当前正在用导入的方案，会回退到星猫键道6。

来源路径和导入结果写在 `settings.ini` 的 `scheme_source`；关掉再开设置程序仍然记得。

## 候选皮肤

候选窗按 `xmjd.candidate-style` v1 文档绘制，和 Mac 版**同一份 JSON**，可以直接互相导入导出：

- 内置：系统简约、星轨、经典竖排、紧凑横排、大字清晰、节庆流光。
- 选中即时生效，下一句输入就用新样式，不需要重新部署。
- **导入…**读入 Mac 版导出的皮肤；**导出…**把当前皮肤存成 JSON，复制到 Mac 版 `CandidateStyles` 目录即可使用。
- 皮肤文件放在 `%LOCALAPPDATA%\XMJD\CandidateStyles`，当前生效的是 `active.json`，其余 `*.json` 作为可选预设；**目录**按钮直接打开。
- 皮肤支持横排/竖排、字号、行高、圆角、内外边距、透明度、最大宽度、编号与注释开关，以及浅色/深色两套配色和选中渐变色。系统深色模式由 `AppsUseLightTheme` 决定。
- 输入法是内嵌编码（编码显示在应用程序里），所以候选窗不重复画编码行；`showPreedit` 字段仍保留在文档里以兼容 Mac。

## 卸载与重新安装

先切换到其他输入法，然后从 Windows 设置的“已安装的应用”卸载 XMJD。卸载先注销两种架构，成功后移除程序文件，保留用户数据。注销失败会停止文件删除，保留脚本和卸载入口供重试。已被应用加载的输入法 DLL 可能要重启后才能清除。

普通安装不覆盖已有版本目录，也不提供自动升级。升级前请先卸载旧版本；安装器检测到已有 XMJD 安装或注册时会拒绝继续，卸载会保留用户词库和设置。若 DLL 丢失或注册不完整，请先切换到其他输入法并退出使用它的应用，再用**同版本**安装包修复。在安装包所在目录的 PowerShell 执行：

```powershell
& '.\XMJD-0.1.5+8-windows-x64-setup.exe' /REPAIR
```

修复会核对现有安装的版本、固定目录和注册所有权，从安装包恢复程序文件，再重建双架构注册，保留用户数据。路径中有链接、存在未知文件、安装属于其他版本或所有权不匹配时会拒绝修复；文件仍被占用时先注销或重启再试。修复后可以重新使用或正常卸载。手动开发安装仍使用其 `scripts/unregister.ps1` 注销。不要直接删除仍在注册中的 DLL。

## 首次实机验收

请先在普通用户的记事本中测试，再测试常用编辑器。

- 中文编码、候选与数字/鼠标选词、空格、翻页、Esc、中文标点。
- 左右 Shift 的中英切换和组合确认；Ctrl+Shift 不应误判为单独 Shift；Caps Lock、Ctrl 快捷键、F4/Ctrl+grave 方案选单。
- 切换字段、窗口和输入法，确认不会重复上屏或把旧组合写入新字段。
- 32 位和 64 位应用、不同 DPI/多显示器、Host 首次启动与中断恢复。

公开 CI 的编译、CTest、真实引擎和安装检查可以覆盖自动验证部分；实际 Windows 11 桌面中的 TSF 输入、应用兼容性、候选定位和按键延迟仍需要实机验收。Windows Server runner 的成功不代表所有桌面场景都已通过。

如需诊断，请在**普通** PowerShell 中执行：

```powershell
$installDirectory = "$env:ProgramFiles\XMJD\0.1.3+4"
& "$installDirectory\XMJDHost.exe" --health
```

`--health` 返回 `ok` 表示当前 Host 可用，`unavailable` 表示尚未就绪或没有运行。该命令只查询。没有运行中的 Host 时可执行 `--deploy`，成功输出 `deploy-ok`；不要在管理员窗口运行 Host。部署失败请保留错误码及 Logs/Staging，不要删除词库。

## 从源码构建

普通用户只需要上面的 EXE。开发构建需要 Windows x64、Visual Studio 2022/2026 的 C++ 桌面工具（含 x64/x86 工具及 SDK）、CMake 3.24 或更新版本、7-Zip 和 64 位 PowerShell。VS 2026 还要求 CMake 支持对应生成器。完整 checkout 必须保留 `Windows/` 与 `xmjd/xmjd.zip` 的相对位置。

在仓库根目录执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\build.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\package-installer.ps1
```

构建脚本检测已安装的 Visual Studio，校验锁定依赖与方案，构建 x64/Win32，分别运行 CTest，合并为带逐文件 SHA-256 清单的交付目录和 ZIP。CI 还运行真实 librime/Lua/OpenCC 候选和上屏测试。任何原生命令失败即停止。安装器使用固定 SHA-256 的 Inno Setup 6.5.4 编译器。

```text
Windows/out/build/x64/Release/         Host、x64 TIP、测试工具、运行时资源
Windows/out/build/x86/Release/         x86 TIP、测试工具
Windows/out/package/                  双架构开发交付及 ZIP
Windows/out/release/                  EXE 安装程序、对应源码及校验值
```

单架构构建使用 `-Architecture x64` 或 `x86`；完整交付必须使用默认 `all`。已有同名输出不会覆盖。校验交付支持目录或 ZIP，也可在其他平台的 PowerShell 执行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\build.ps1 -VerifyPackage ".\Windows\out\package\XMJD-0.1.5+8-windows-x64-x86-Release.zip"
```

开发安装需要管理员的 64 位 PowerShell：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\install.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Windows\scripts\register.ps1 -InstallDirectory "$env:ProgramFiles\XMJD\0.1.3+4"
```

注册固定使用 HKLM 双位数 COM/TSF 视图。两种 TIP 从 HKLM Registry64 的 `Software\XMJD\HostExecutablePath` 读取 Host；完整注册还包含 `RegistrationComplete` 与带引号的登录启动项。重复完整注册不再执行；失败逆序回滚，包括失败的架构。路径所有权不匹配时拒绝修改。安装器不会启动提权 Host。

## 架构与依赖

TIP 只处理 TSF 编辑会话和候选 UI，通过同一登录会话的本地管道连接 Host。输入事务共享 50 ms 截止时间；不确定结果会使连接失效，不重放按键，不应用迟到回复。上下文、会话和候选 revision 独立校验，旧组合不转写新字段。候选支持应用接管 UI 和独立非激活窗口。

`librime.lock.json` 固定 librime **1.16.1**、core `de4700e9f6b75b109910613df907965e3cbe0567`、librime-lua `68f9c364a2d25a04c7d4794981d7c796b05ab627`、Lua **5.4.8**、OpenCC **1.1.9** 和工件/许可证哈希。Windows 原生代码按 GPLv3 分发；官方引擎含 GPL-3.0 的 octagram。Release 同时提供第三方对应源码和原始许可证，详见 `THIRD_PARTY_NOTICES.txt`。源码包脚本保留上游构建文件和准确提交来源。

设置伴侣、语音/AI、词库同步、自动更新、ARM64 尚未实现。当前交付目标是可安装的独立输入法和可追踪的 Windows 构建证据。`Windows/VERSION` 是 Windows 的唯一版本输入；Apple 平台不包含在此独立仓库中。
