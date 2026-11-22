# Standalone Windows Input Method — Implementation Handoff

## Delivery Status

The owner authorized M0-M2 implementation on macOS, then explicitly authorized a
separate public Windows repository and a directly installable GitHub Release on
2026-11-16. Version 0.1.3+4 added the missing first-use surface requested after
testing 0.1.2+3: a native settings program with scheme selection, engine status,
explicit deployment and diagnostics. Version 0.1.4+5 answers the follow-up request
for Mac parity in configuration depth: an external Rime scheme source directory
and a shared candidate-skin format with the Mac build. It retains the Inno Setup installer, actual
Windows CI, and binary/source release attachments. The repository is
https://github.com/Attiv/xmjd-windows. Validation evidence and remaining interactive
acceptance gates are documented in `Windows/README.md`.

## Goal

Build an independently branded and installed Windows edition of XMJD, using
librime and the existing input schemes, with the macOS edition as the functional
reference. Implement an original Windows TSF frontend, not a Weasel fork or
reskin.

## Owner-Confirmed Facts

- The existing input scheme already works on Windows and is used by the owner.
- Revalidating the scheme or distributing a renamed Weasel is not the task.
- The original planning handoff has since been followed by an authorized M0-M2
  implementation round; this document records architecture and acceptance gates.

## Invariants

- Preserve iOS/macOS behavior, Xcode settings, versions, and existing user edits.
- Reuse scheme resources without rewriting the input algorithm or Lua modules.
- Never write into an existing Weasel installation or its user-data directory.
- Preserve existing Apple behavior and user edits. The owner separately approved
  publishing the Windows-only repository and installer; do not export Apple code.

## Planning Assumptions

- Initial target: Windows 11 on x64 hardware, with both x64 and x86 TSF DLLs.
- Windows ARM64, Windows 10, automatic updates, and Store distribution are not
  first-release requirements.
- Full Mac feature parity is phased; reliable independent typing is the first
  acceptance gate.

## Readiness

M0-M2 source/developer delivery was completed at 0.1.1+2. The direct-install
public delivery round started at 0.1.2+3; 0.1.3+4 added first-use settings and
scheme selection; 0.1.4+5 adds external scheme sources and shared candidate skins.
This
includes native input/candidates, Host startup/recovery, resource and dependency
validation, package manifests, and installation/registration scripts. M0-M2
runtime acceptance remains open until verified on Windows. Do not repeat a
scheme feasibility investigation. Windows system integration needs real testing.

## Requirements and Completion Gates

| ID | Requirement / source | Observable acceptance |
|---|---|---|
| FR-01 | Independent Windows product; explicit owner request | Its own name, icon, CLSID, profile GUID, installer and data directory; works with Weasel absent. |
| FR-02 | Original frontend, not a renamed Weasel; explicit owner request | Own TSF implementation; no Weasel source, binaries, IPC service, installation or runtime dependency. librime is allowed. |
| FR-03 | Existing scheme works; owner-confirmed invariant | Package the existing resources unchanged; new work concerns integration, not replacing the input algorithm. |
| FR-04 | Mac input behavior as reference; inferred first milestone | Composition, candidate selection, paging, cancellation, English mode and context changes work in the M2 test matrix. |
| FR-05 | Native settings and scheme management; recommended M3 | Native settings, isolated imports, explicit deployment, non-destructive upgrade and uninstall. |
| FR-06 | Voice and AI parity; recommended M4 | API/local speech, profiles, shortcuts, selected/whole-field translation, cancellation and safe delivery have recorded tests. |
| FR-07 | Personal dictionary interoperability; recommended M5 | Windows and existing clients share the exact v2 operation format and conflict semantics, verified with fixtures and a real server. |
| NFR-01 | Preserve existing platforms; project instructions | No iOS/Mac behavior, version, signing or Xcode changes; preserve the owner's uncommitted edits. |
| NFR-02 | Input reliability; architectural requirement | No cross-field or duplicate commits; unavailable helpers do not hang or crash host applications. |
| NFR-03 | Reproducible Windows delivery; project quality bar | Pinned dependency provenance, x86/x64 DLLs, one build entry point, installer and honest verification report. |

M0-M2 are the first delivery. M3-M5 are a staged parity roadmap, not features to
silently include in the first implementation round. The owner can reprioritize
later milestones without changing the original-TSF requirement.

## Chosen Architecture

Use native C++ for the input path and an out-of-process C# WinUI 3 companion for
settings/assistant features. Do not use Swift-on-Windows, Electron, WebView, a
global key-hook imitation IME, or a new cross-platform rewrite of the Apple apps.

```text
Host application, x64 or x86
  -> XMJDTip.dll, matching host bitness
       TSF/COM lifecycle, composition, caret, candidate UI
  -> bounded local IPC
  -> XMJDHost.exe, x64, ordinary per-user desktop process
       singleton librime runtime, per-client sessions, deployment/storage

XMJDDesktop.exe, x64, C# / WinUI 3, introduced in M3
  -> settings/control IPC -> XMJDHost.exe
  -> M4: microphone, speech/API requests, shortcuts, assistant result UI
  -> M5: sync transport; host owns coordinated local dictionary installation
```

- `XMJDTip.dll` is a real TSF text service, not a keyboard hook. Keep .NET,
  WinUI, recording, models, network calls and deployment out of host processes.
- `XMJDHost.exe` owns the only writing librime runtime for a data root. An x86
  TIP can communicate with the x64 host without a second librime build. The x86
  build emits the TIP (and applicable tests); host/companion/librime stay x64.
- The host is not a Windows Service and must not run as SYSTEM/admin. Launch it
  in the ordinary user's logon session via the HKLM Run entry or activation outside `DllMain`;
  never wait for process startup on the key path. First activation may be
  unavailable while the host starts; subsequent input must recover normally.
- Close a host's sessions on pipe disconnect. A broker instance is scoped to
  the interactive logon session; an exclusive data-root writer lease prevents a
  second session for the same user from opening the same learned database.
  Simultaneous sessions for one account are not an initial support claim.
- Companion settings/assistant failure must not stop ordinary Rime typing.
- Candidate display uses a small native Win32 window (DirectWrite/Direct2D as
  needed), not a WinUI runtime loaded into third-party applications. Implement
  TSF candidate UI-element negotiation and respect UI-less hosts.

### Supported Host Boundary

The first release targets ordinary full-trust desktop applications on Windows
11 x64, including x86 desktop processes. A packaged desktop app is not
automatically an AppContainer app.

AppContainer/restricted hosts require a separate early IPC compatibility check:
TSF DLLs inherit host restrictions, and arbitrary cross-package pipes are not a
general supported assumption. Do not advertise support merely by adding a TSF
category. Do not weaken global ACLs, elevate the broker, exempt other apps, or
disable security features to make a test pass. Record a supported mechanism
before adding those hosts to the support matrix; otherwise mark them unsupported
and keep the desktop milestone moving. Secure desktop/UAC and game/anti-cheat
compatibility are outside the initial target.

## Repository Placement and Reference Map

The Windows tree includes native protocol/Broker, x64 Host, x64/x86 TSF TIP,
CMake targets, pinned dependency lock, build/install/register scripts, tests, and
README. Direct-install packaging is included by the owner's 2026-11-16 release
request. The desktop companion remains a later milestone.

```text
Windows/
  IMPLEMENTATION_PLAN.md     # architecture and acceptance handoff
  CMakeLists.txt             # native TIP + host + native tests
  VERSION                   # single Windows version/build source
  Native/Tip/               # implemented COM registration, TSF, composition, candidates
  Native/Host/              # librime owner, client sessions, maintenance
  Native/Protocol/          # small versioned IPC contract
  Desktop/                  # C# WinUI companion, starts in M3
  Tests/                    # protocol/native tests and portable golden fixtures
  Installer/                # direct-install EXE, authorized in the 0.1.2+3 delivery
  scripts/                  # dependency preparation/build/register/package
  README.md                 # exact build, recovery and manual verification
```

| Existing evidence | Reuse on Windows |
|---|---|
| `macOS/MacInputController.swift`: `handle`, `finishComposition`, `resetSession`, `renderContext` | Behavior and context-ownership checks, not IMK calls. |
| `Keyboard/RimeEngine.swift`: `RimeEngine`, `CandidateItem`, `RimeContextResult`; `Keyboard/RimeBridge.h` | C API usage and snapshot shape; replace Swift wrapper with native C++ ownership. |
| `macOS/MacRimeService.swift`: `MacRimeService` | One runtime, independent sessions, maintenance gating and schema readiness checks. |
| `macOS/MacCandidatePanel.swift`, `macOS/MacCandidateStyle.swift` | Candidate behavior and style semantics, not AppKit drawing or Mac font names. |
| `xmjd/xmjd.zip`, `xmjd/SchemaImporter.swift`, `Shared/RimeDefaultConfiguration.swift` | Existing scheme/resources; port only needed import/default-generation rules. |
| `macOS/MacAssistantController.swift`, `macOS/MacFocusedText.swift`, `macOS/MacGlobalShortcuts.swift`, `macOS/MacSpeechCapture.swift` | Operation tokens, delivery invariants and capture workflow; Windows APIs are new. |
| `Shared/AITextGenerationProtocol.swift`, `Shared/VoiceTranscriptionProtocol.swift`, `Shared/LocalVoiceProtocol.swift` | Endpoint/payload/profile semantics; create protocol fixtures instead of pretending Swift sources are portable. |
| `xmjd/LocalSpeechModelStore.swift`, `xmjd/SherpaSpeechRecognizers.swift` | Model catalog and recognition behavior; use pinned Windows sherpa-onnx/ONNX binaries only after compatibility is verified. |
| `Shared/PersonalDictionarySyncDocument.swift`, `Shared/PersonalDictionaryFileAccess.swift`, `xmjd/DynamicPhraseWebDAVSync.swift` | Exact v2 merge/recovery rules; replace Darwin locks and Keychain integration. |
| `.trellis/spec/frontend/macos-input-method.md`, `macos-settings.md`, `macos-assistant.md`, `personal-dictionary-webdav.md` in the same directory | Read relevant contracts, preferring current source and latest appended policy over stale overview text. |

## Contracts That Must Exist Before Coding Across Boundaries

### TSF / Composition

- Implement COM exports/class factory and `ITfTextInputProcessorEx`, key/focus,
  composition and candidate UI-element integration using Microsoft TSF guidance.
  Official sample scaffolding is allowed with license/provenance retained; do
  not copy its obsolete build assumptions or substitute its input engine.
- Generate and persist XMJD-owned CLSID/profile GUIDs once. Register x86 and
  x64 independently with the same identity; never reuse Weasel/SampleIME IDs.
- `OnTestKeyDown`/`OnTestKeyUp` do not mutate Rime. A delivered key is processed
  once, with a documented modifier/release policy. Preserve shortcuts and
  unhandled events; no hard-coded candidate digits/page keys replacing the schema.
- Own one Rime session per live TSF input context; attach an epoch to every
  activation/context change. No shared composition between windows or fields.
- Edit host text only inside an appropriate `ITfEditSession`. Check both the
  request HRESULT and edit-session outcome; asynchronous scheduling is not a
  successful insertion. Release interfaces/sinks correctly and handle reentry.
- Drain Rime commits once per operation. Use real TSF composition ranges and
  convert UTF-8 caret offsets to UTF-16 safely, including non-BMP characters.
- Explicit confirmation uses converted output with raw fallback. Explicit
  input-mode/profile switching finalizes pending raw encoding where the original
  live context permits it. Focus loss/host termination must never retarget a
  commit to a newly focused field. The actual Windows callback mapping needs
  tests, not a literal copy of IMK callback names.
- Candidate clicks carry the current context epoch and snapshot revision.
  Stale clicks, queued edit sessions and replies are ignored after invalidation.
- `ITfThreadMgrEventSink` handles document/context focus changes immediately;
  old composition cleanup retains only the original context/range. Shift edges
  and scheme switcher keys reach Rime. Modifier kNoop can still emit commit_code:
  drain that snapshot once, advance revision and pass the native modifier through.
  Ordinary rejected keys retain non-draining snapshots.

### IPC v1

Use local named pipes for the initial full-trust desktop path. Keep the wire
contract independent of C++ layout and pointer size: length-prefixed UTF-8 JSON
with a 1 MiB frame limit is sufficient initially. Validate size before allocation,
UTF-8/field types before use, and operation-specific limits. Keep one outstanding
input operation per session and a small bounded queue per connection.

```text
request:  version, connectionEpoch, sessionId, contextEpoch, sequence, operation, payload
response: matching identities, status, handled,
          commitText, preedit, caretUtf8Offset, candidates, page, snapshotRevision
input:    CreateSession, ProcessKey, SelectCandidate, Finalize, Cancel, DestroySession
control:  Health, GetSettings, ApplySettings, Deploy
errors:   unavailable, busy, stale, unsupportedVersion, invalidRequest, engineFailure
```

- Session IDs are bound to their connection; do not trust arbitrary IDs/PIDs in
  JSON. Duplicate sequence numbers cannot process a key twice. Connection epochs
  invalidate all pending results after restart. No protocol pointers/COM objects.
- No network or disk work on per-key callbacks. Start with a 50 ms input IPC
  deadline and a warmed p95 <=10 ms key-to-commit/edit-request target on the test
  machine; record hardware and actual timings. These are acceptance targets,
  not measured results. Do not hide a slow path by increasing the deadline.
- On timeout, invalidate the connection/session; never replay an uncertain
  `ProcessKey` or apply a late reply. Preserve last acknowledged raw preedit and
  define/test one recovery path in the original live context. Pass through a
  not-yet-consumed original key only when no engine response has been applied;
  never both commit its delayed result and pass the key. If the original context
  is gone, do not insert elsewhere; expose recoverable text instead of claiming
  lossless recovery. Include failure between processing and reply in tests.
- Reject remote clients. Specify least-privilege pipe ACLs, authenticate the
  connected token/logon identity, and verify the expected server identity/path;
  do not use an Everyone-writable endpoint. Input clients cannot submit arbitrary
  paths, launch commands or privileged maintenance operations. Keep maintenance
  endpoints unavailable to restricted input clients.
- A verified elevated desktop client in the same user, session and logon may
  connect to the ordinary Host in the protected install directory. It may use
  input sessions, not engine control operations. Pipe clients provide
  identification-level security context only; the Host must not act as them.

### Storage / Dependencies / Installation

- Managed writable root: `%LOCALAPPDATA%\XMJD\`, containing `SharedData\Rime`,
  `UserData\Rime`, `Staging`, `Backups`, and later `VoiceModels`. Resolve known
  folders using platform APIs. Never use `%APPDATA%\Rime` or edit Weasel data.
- Pin librime, Lua plugin, Lua/OpenCC and native dependency versions/ABI; use
  verified official Windows artifacts when available, otherwise a reproducible
  manifest build. Record hashes/licenses. Apple dylibs/XCFrameworks are unusable.
- One host owns runtime setup/finalization and serialized Rime calls. Client
  destruction cannot finalize other clients. Maintenance makes new input
  unavailable, detaches sessions, takes an exclusive writer lease, stages and
  validates resources, then publishes readiness. Never deploy underneath typing.
- Import/deploy failures restore original resources and preserve learned data;
  startup recovers unfinished journals before opening Lua or learned databases.
- Delivery includes a dual-architecture development ZIP and an Inno Setup EXE
  that invokes checked registration and unregistration scripts. Install the complete package
  under `%ProgramFiles%\XMJD\<version>` with `Tip\x64` and `Tip\x86`, then register
  both HKLM COM/TSF views. Installation, registration and unregistration require
  administrator 64-bit PowerShell; building and actual input use a non-elevated
  desktop session. Both TIPs read the Host path from HKLM Registry64
  `Software\XMJD\HostExecutablePath`. HKLM Run starts the Host on ordinary logon;
  scripts must not launch it elevated. No third-party files go in `%WINDIR%\IME`.
- Registration never silently changes the user's default input method. Document
  enable/select steps and support unregister without touching other inputs.
- Loaded DLLs require side-by-side versioned staging/deferred cleanup; do not
  terminate user apps to replace files. Failed install/registration rolls back
  only XMJD changes. Uninstall preserves user data by default; deletion is an
  explicit separate choice. Public binaries require a real signing plan.
- `Windows/VERSION` is the only Windows version/build source. Start at 0.1.0 /
  build 1 when implementing; increment once per Windows change round. Do not
  bump iOS/macOS for a Windows-only or documentation-only round.

## Phased Implementation Plan

| Gate | Scope / requirement IDs | Modules / dependencies / risk | Required proof |
|---|---|---|---|
| M0: Windows toolchain and identity | FR-01/02, NFR-01/03 | Native scaffold, dependency manifest, version and registration; Windows VM/PC + SDK/MSVC/CMake; medium | Build/load/register/unregister x86+x64 TIP; XMJD appears as its own profile; Weasel absent; no app/data changes outside XMJD. Audit tools first, do not reinstall working tools blindly. |
| M1: End-to-end input core | FR-03/04, NFR-02 | Host runtime, bounded IPC, TSF composition/key path; M0; high | Existing packaged scheme produces preedit/candidates/commits via own TIP; two apps/two fields have isolated sessions; host failure doesn't hang apps; no reinterpretation of scheme feasibility. |
| M2: First usable delivery | FR-04, NFR-02/03 | Candidate panel, caret/DPI, ASCII mode, shortcuts, context finalization; M1; high | Pass desktop test matrix below, include x86 process; repeatable build/register/recovery scripts; record IPC timing and failure-injection results. Stop here for first owner acceptance. |
| M3: Product shell and installer | FR-05, NFR-03 | WinUI companion, config/control IPC, import/backup/deploy, installer; accepted M2; medium/high | Settings launch separately, closing them leaves typing working; fresh install/upgrade/rollback/uninstall tested; scheme import cannot overwrite unrelated data. |
| M4: Voice and AI parity | FR-06, NFR-02 | Companion audio, HTTP/providers, model cache, shortcuts, UI Automation; M3; high | API speech first, then local Zipformer/SenseVoice; profile/prompt parity; no unsupported-control writes; focus/content-change fallback and no duplicate delivery verified. |
| M5: Personal dictionary sync | FR-07, NFR-02 | Shared fixtures, merge/transport, Windows locking/journal, settings; M3 and stable storage; high | Golden fixtures match existing semantics; real WebDAV tests including conditional writes/conflicts; Windows <-> existing Mac/iPhone interoperability report. |

### M3 UI and Packaging Decisions

- Use an unpackaged C# WinUI 3 companion with an explicit Windows App SDK/.NET
  runtime deployment strategy in the installer. Scaffold from a verified official
  template; exact SDK/runtime versions must be pinned on the Windows machine.
- Settings use native controls and theme resources, logical keyboard navigation,
  accessible names, light/dark/high-contrast support, and a narrow-window layout.
  Candidate windows never activate or steal the target field's focus. Honor TSF
  UI-less `pbShow` decisions rather than forcing an always-on-top overlay.
- Start with system-style candidates and needed style fields; map unavailable
  Mac fonts to Windows fallbacks. Do not promise pixel-identical Mac rendering
  or silently reinterpret unsupported style JSON. Validate before applying.
- No animation work in the initial scope. No bundled web frontend or new custom
  design system. Detailed screen layouts/visual polish are not designed here.

### M4 Delivery Safety

- Use Windows audio capture, appropriate hotkey APIs, Credential Manager/DPAPI
  and UI Automation. Prefer `RegisterHotKey` for supported combinations; add a
  targeted modifier gesture implementation only when parity actually needs it.
- Match AI/STT request formats and profile semantics from the cited Swift
  protocols with request fixtures. No API keys in plain settings, scheme files,
  TIP IPC, logs or diagnostics. Ordinary input never triggers network requests.
- Model/HTTP/audio work runs outside TSF and the engine key executor. Bound
  capture/request lifetime and cancel old operations before changing settings.
- Bind each request to operation ID, foreground process/window, target element,
  text/selection snapshot and observed changes. New composition, focus change,
  cancellation or incompatible target invalidates automatic insertion.
- `TextPattern` is not a write API. Auto-write only with a verified supported
  path and unchanged target. Do not use blind Ctrl+A/Ctrl+V, arbitrary SendInput,
  or admin elevation as a whole-field replacement fallback. Password/secure or
  inaccessible controls reject capture/replacement.
- Following current Mac voice behavior, preserve successful recognition after
  target changes via one clearly indicated clipboard/result fallback. Translation
  may preserve its result for manual copying but must not replace a new field.
  Report a clipboard change rather than silently implying insertion succeeded.
- A text-write success followed by caret failure is not a reason to write again.

### M5 Wire and File Compatibility

Port the contract in `.trellis/spec/frontend/personal-dictionary-webdav.md`, not
just the UI. Keep `xmjd6-personal-dictionary-v2.json`, numeric transaction ordering,
within-transaction row order, tombstones/restores/reset conflict rules and exact
six-file local installation allowlist. This is not generic Rime userdb sync.

Use Windows file-handle locking and atomic replacement/journal recovery instead
of Darwin `flock`. Host pause/release and an exclusive local lease must precede
installation. Preserve the 8 MiB remote bound, HTTPS and redirect rejection,
strong ETags, conditional PUT, and maximum three 412 merge attempts. An accepted
remote upload cannot be rolled back by pretending a later local install failed.

## Validation and Rollback

Automated tests start with native assert/CTest checks; do not add a second test
framework without need. Each non-trivial boundary needs a small executable test.

| Area | Good/base case | Failure/edge case |
|---|---|---|
| Registration | One logical profile, both bitnesses, explicit enable | Partial x86/x64 failure rolls back; unregister twice is safe; Weasel untouched. |
| TSF/key routing | Schema-selected commit/page/ASCII/punctuation | Test callbacks don't mutate; Ctrl shortcuts pass; keyup/Caps/dead keys/non-US layout are tested or listed unsupported. |
| Ownership | Switch between two fields/windows/apps | Old candidate click, queued edit session, profile switch, focus loss and app close never commit to the new target. |
| Unicode/caret | CJK, ASCII, emoji/non-BMP and candidate comments | Partial UTF-8 offsets, clipped caret rectangles, 100/150/200% DPI, mixed-DPI monitors, negative coordinates. |
| IPC | x86/x64 same protocol, one ordered key stream | Malformed/oversize frame, wrong version/session/epoch, duplicate sequence, timeout after processing, host restart and unavailable startup. |
| Persistence | Learning survives restart and upgrade | Busy data root, read-only/full disk, failed deployment, crash mid-install and recovery; never delete learned DBs as a repair shortcut. |
| Companion | Native settings save; typing continues after close | Companion crash, missing runtime, cancelled model download, invalid style/import; input still works. |
| Assistant | Valid recognition/translation to unchanged supported field | Focus/content/selection change, new composition, API timeout, microphone loss, caret failure after write, protected control. |
| Sync | Fixture merge is idempotent and cross-client compatible | Row-order conflict, reset mismatch, 412, bad ETag, redirect, oversize, upload accepted/disk failure, interruption recovery. |

M2 manual matrix: Windows 11 x64 with architecture/build recorded; a minimal x64
Win32 Edit/RichEdit harness, an x86 harness, Notepad, Edge/Chrome, VS Code/Electron,
and one real chat editor used by the owner. Test Office when available; do not
mark an uninstalled app passed. Include profile switching, Esc, candidate mouse
selection, repeated activation, mixed DPI and broker failure. Restricted hosts,
elevated apps, remote desktop and games each need separate evidence before a
support claim. VM checks do not prove microphone/multi-monitor hardware behavior.

Implemented script contracts: `Windows/scripts/prepare-deps.ps1 -Architecture x64`
(with `-VerifyOnly`), `build.ps1 -Architecture all|x64|x86 -Configuration Debug|Release`
(default all/Release), `build.ps1 -VerifyPackage <directory-or-zip>`,
`install.ps1 -Configuration Debug|Release [-PackageDirectory <path>]`, and
`register.ps1` / `unregister.ps1 -InstallDirectory <path>`. Dependencies and all
prepared files are verified against locked provenance/content; build uses the
Visual Studio 2022 generator. Both architectures run CTest before merged CMake
installation, manifest generation and ZIP verification. Deliveries are named
`Windows/out/package/XMJD-0.1.1+2-windows-x64-x86-Release` and `.zip`.

Pipeline checkpoints: detect tools -> prepare/verify pinned dependencies -> build
both TIP architectures -> CTest -> assemble and verify development directory/ZIP
-> Inno Setup EXE -> real administrator install/register/uninstall checks ->
ordinary-user Host and interactive typing tests. Corresponding third-party source,
project source and SHA-256 files accompany the public installer.
Registration checks both architectures and ownership before changes, is a no-op
for the identical complete installation, and rolls back only this attempt in
reverse order. Partial cleanup retains ownership metadata for a matching retry.
See `Windows/README.md`. Publishing, default-input changes and destructive data
cleanup are never automatic. CI and native runtime/TSF gates require Windows.

Rollback restores prior XMJD binaries/registration and the prior resource
snapshot, never force-kills host apps, removes other input methods or erases
user data. Do not downgrade a changed data format without an explicit migration
and backup plan. No data-format migration is required for M0-M2.

Every milestone report must separate: files changed, requirements completed,
commands actually run, automated results, real-Windows manual results, skipped
checks and remaining risks. Logs contain timing/status/error codes only, never
ordinary keystrokes, composed text, credentials or captured audio.

## Instructions for the Receiving AI

1. Preserve pre-existing edits, especially `xmjd.xcodeproj/project.pbxproj` and
   `.claude/`. Never modify Apple source to make Windows scaffolding easier.
2. Keep implementation within M0-M2. The input scheme is owner-confirmed working;
   do not propose a Weasel fork or repeat scheme-feasibility investigation.
3. On macOS-only tooling, verify only what can be honestly checked. Native Windows
   build/runtime gates remain unverified until an actual Windows run; portable
   tests and mocks do not establish Windows IME success.
4. Keep API/IPC/registration/storage contracts synchronized with code and scripts.
   The companion remains a later milestone; the installer is now authorized.
5. Return build, install, enable/unregister instructions, actual evidence and
   open failures. Keep interactive owner acceptance distinct from CI success.

## Official Research Sources

These sources establish platform constraints, not that the product has passed
Windows build or runtime acceptance. Microsoft sample code is an API reference;
its old toolchain/instructions need current Windows verification.

- [Custom IME requirements: TSF, host restrictions, branding and UI](https://learn.microsoft.com/en-us/windows/apps/develop/input/input-method-editor-requirements)
- [TSF x86/x64 registration and install considerations](https://learn.microsoft.com/en-us/windows/win32/tsf/64-bit-platform-considerations)
- [RequestEditSession: lock/synchronous/asynchronous outcomes](https://learn.microsoft.com/en-us/windows/win32/api/msctf/nf-msctf-itfcontext-requesteditsession)
- [Candidate UI-less negotiation](https://learn.microsoft.com/en-us/windows/win32/tsf/uiless-mode-overview)
- [Microsoft IME sample README](https://github.com/microsoft/Windows-classic-samples/blob/main/Samples/IME/README.md)
- [librime Windows build](https://github.com/rime/librime/blob/master/README-windows.md)
- [Windows IPC and packaged/AppContainer distinctions](https://learn.microsoft.com/en-us/windows/apps/develop/communication/interprocess-communication)
- [Named pipe security](https://learn.microsoft.com/en-us/windows/win32/ipc/named-pipe-security-and-access-rights)
- [UI Automation text patterns cannot themselves write text](https://learn.microsoft.com/en-us/dotnet/framework/ui-automation/ui-automation-textpattern-overview)
- [Pinned WinUI design/development skill](https://github.com/openai/skills/tree/49f948faa9258a0c61caceaf225e179651397431/skills/.curated/winui-app)

## First-Use Settings and Scheme Selection (0.1.3+4)

The 0.1.2+3 first-use report was accurate: it installed only the input core, left
one language-bar icon and had no settings window or user-visible scheme picker.
The fallback configuration also enabled only `xmjd6`, and the engine selected
`xmjd6` unconditionally when creating a session.

0.1.3+4 closes that usability gap:

- `XMJDSettings.exe`, a native Win32 dialog installed with the product and linked
  from the Start Menu. The installer offers to open it as the original ordinary
  user after installation.
- It lists the deployed schemes by reading `Resources\Rime\*.schema.yaml`, hides
  auxiliary schemes marked 勿选, shows Host health, and offers apply/deploy,
  redeploy, start engine, open user data, open logs and Windows language settings.
- Applying a scheme writes `%LOCALAPPDATA%\XMJD\settings.ini` and updates the
  user-level `default.custom.yaml` `schema_list` while preserving other patches.
  The selected scheme is placed first. Switching F4/Ctrl+grave in a live session
  is written back when that session is destroyed.
- Host protocol v1 adds a control-only `deploy` operation. The Host destroys
  active engine sessions, runs Rime maintenance, validates every listed scheme,
  reloads the selected scheme and resets input pipe connections so TIPs recreate
  sessions against the new scheme. The 50 ms key path remains unchanged.
- A deploy marker lets the next Host startup perform maintenance even when the
  settings program cannot reach a running Host. `XMJDSettings.exe --check` and
  `--select <schema>` provide deterministic installer-CI coverage.

Real Windows CI must still verify the new dialog binary, settings persistence,
scheme-list generation and the pinyin redeploy/commit path, plus the existing
install/repair/uninstall gates. Desktop TSF typing and DPI/candidate placement
remain owner acceptance.

## External Scheme Sources and Shared Skins (0.1.4+5)

The owner asked for the Mac configuration surface on Windows: choosing the scheme
path and picking a skin. Both are implemented without copying Mac-only plumbing.

Scheme source:

- The settings dialog scans `Resources\Rime` plus `%LOCALAPPDATA%\XMJD\UserData\Rime`
  (recursive, skipping `build`), so imported schemes appear next to bundled ones.
  User-directory entries shadow bundled entries with the same `schema_id`.
- Choosing a directory validates that its root contains `.schema.yaml`, rejects the
  product data directory or its ancestors/descendants, and copies only resource
  extensions (`yaml`, `lua`, `json`, `ocd`, `ocd2`, `txt`, `tsv`, `gram`, images)
  into `UserData\Rime\imported\<folder>`, skipping `build`, `*.userdb`,
  `user.yaml`, `installation.yaml`, `trash` and links. Limits are 20,000 files and
  512 MiB. The source directory is never modified.
- The absolute source path is persisted as `scheme_source` in `settings.ini`;
  `--import-scheme <dir>`, `--schemas` and `--remove-schemes` expose the flow to CI.
- Removing imports deletes only `UserData\Rime\imported`; if the active scheme came
  from there the selection falls back to `xmjd6` and `schema_list` is rewritten so
  the next deploy never references a missing file.
- `RimeRuntime::validateSchemas` now treats only `xmjd6` and the selected scheme as
  hard requirements. A third-party scheme that fails to compile is listed in
  `Logs\schema-skipped.log` instead of taking the whole engine down.

Candidate skins:

- `Native/Common/CandidateStyle.{h,cpp}` implements the same `xmjd.candidate-style`
  v1 document as `macOS/MacCandidateStyle.swift`, including the optional
  decoration colours, so skins exported by either platform import on the other.
  A hand-written, locale-independent JSON reader/writer avoids new dependencies;
  `Tests/CandidateStyleTests.cpp` round-trips the real `macos-default.json`
  geometry and colours and checks the layout math.
- Six built-ins mirror the Mac presets: 系统简约, 星轨, 经典竖排, 紧凑横排, 大字清晰,
  节庆流光. The active document lives at `CandidateStyles\active.json`; extra
  `*.json` files become pickable presets. Import/export and a directory shortcut
  live in the settings dialog.
- `Native/Tip/CandidateRenderer.{h,cpp}` replaces the fixed system-colour listbox:
  it scales by monitor DPI, flows horizontal candidates with wrapping, paints
  rounded gradients, selected-item gradients, label/comment colours and opacity
  through GDI+ into a 32-bit DIB and presents it with `UpdateLayeredWindow`, and
  hit-tests clicks against the same layout. Dark/light follows
  `AppsUseLightTheme`. Because TSF composition is inline, the window never draws a
  duplicate preedit row; the field is still parsed and serialized for Mac parity.
- Selecting a skin writes `active.json` immediately, so the next composition uses
  it without a redeploy.

Outstanding: real desktop verification of the painted popup (placement, DPI,
click targets), settings-dialog layout at 100%/150% scaling, and a skin edit round
trip between the two platforms.

## Public Windows Delivery Evidence (0.1.2+3)

- Public Windows-only repository: https://github.com/Attiv/xmjd-windows. The owner
  authorized direct installation and GitHub Release delivery on 2026-11-16.
- Actual Windows/MSVC builds have passed for x64 and x86, including x64 CTest
  4/4 and x86 CTest 3/3. Real librime/Lua/OpenCC deployment, candidate selection,
  one-shot commit consumption, cancel/finalize, and simplified/traditional
  conversion have passed on hosted Windows runners.
- Windows PowerShell 5.1 assembled and verified both the package directory and
  ZIP. COM exports use STDAPI and olectl.h; SDK TSF identifiers link through uuid,
  not a nonexistent msctf.lib. The build detects VS 2022 and VS 2026 generators.
- The Inno Setup 6.5.4 EXE has built and installed on hosted Windows. An ordinary
  non-administrator, non-session-zero account has deployed the installed engine,
  started Host and passed authenticated-pipe health checks.
- Same-version `/REPAIR` restores damaged program files only after checking fixed
  path, Inno uninstall ownership, manifest version and absence of reparse points.
  Failed cleanup retains program files and user data. Missing-DLL recovery,
  final uninstall and offline corresponding-source expansion are CI release
  gates; their final result and Release link are recorded after completion.
- Interactive Windows 11 TSF typing, application/DPI compatibility and measured
  input latency still require owner acceptance. CI does not establish those.

## Historical Source Handoff Evidence (0.1.1+2)

The entries below describe the earlier macOS-only source handoff, before the
public Windows build and installer round. Its pending statuses are historical.

- This handoff retains the original research notes above for provenance; they do
  not constitute Windows build or runtime validation.
- Version remains 0.1.1+2 for this implementation round. Native TSF composition,
  candidate negotiation, context/session invalidation and asynchronous Host
  startup are implemented; native build/runtime acceptance is not yet recorded.
- Official librime 1.16.1 artifacts were extracted and verified on macOS:
  184 prepared files, 30 OpenCC files, 15 license files. The lock records the
  built-in Lua plugin commit `68f9c364a2d25a04c7d4794981d7c796b05ab627`, Lua 5.4.8,
  OpenCC 1.1.9, source/license hashes and the existing scheme archive checksum.
  The bundled octagram GPL-3.0 obligations remain a public-distribution gate.
- Portable CMake build and CTest passed on macOS ARM64 (`ProtocolTests`, 1/1),
  including modifier kNoop commit consumption. Strict C++17 compilation and
  ASan/UBSan also passed. Commands are listed in README. These tests cover
  codec/Broker/Unicode and operation ownership, not native TSF integration.
- PowerShell 7.7.0 on macOS was used for parsing and temporary package/build
  orchestration fixtures. Dangerous archives, content tampering, package
  directory/ZIP checks and isolated CMake resource/install rules were exercised.
  Temporary Win32/Rime substitutes exercised Host one-shot commits, API bounds,
  resource deployment recovery and missing-resource rejection. Substitutes are
  not a replacement for actual Windows execution. Registration fixtures cover
  first/repeated operations, reverse rollback, cleanup-failure ownership and
  matching retry. Extracted TIP key methods passed against SDK virtual-key
  constants with mocked keyboard state. Thread/profile advice-failure cleanup
  has native COM test coverage in source, awaiting Windows execution. Final
  review fixed distinct Win32 integer-type expressions and added the common
  target's uuid import for KnownFolder identifiers.
- Windows/MSVC compilation, Windows PowerShell 5.1 execution, real librime/Lua
  initialization, COM/TSF registration, typing, visuals, IPC/latency benchmarks,
  signing and public release have not been performed. No animation is included.
