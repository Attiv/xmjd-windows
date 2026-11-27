#include "../Native/Host/RimeRuntime.h"
#include "../Native/Common/Pipe.h"
#include "../Native/Common/CorrectionPreferences.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>

static_assert(sizeof(void*) == 8, "RimeRuntimeTests 只支持 x64 构建");
#if !defined(_M_X64) && !defined(__x86_64__)
#error RimeRuntimeTests 只支持 x64 架构
#endif

namespace {
using xmjd::Snapshot;
using xmjd::win::RimeRuntime;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string describe(const Snapshot& snapshot) {
    std::string result = "raw=[" + snapshot.rawInput + "], preedit=[" + snapshot.preedit +
        "], commit=[" + snapshot.commitText + "], ascii=" + (snapshot.asciiMode ? "true" : "false") +
        ", candidates=" + std::to_string(snapshot.candidates.size());
    for (const auto& candidate : snapshot.candidates) result += " [" + candidate.text + "]";
    return result;
}

void requireEmpty(const Snapshot& snapshot) {
    require(snapshot.rawInput.empty() && snapshot.preedit.empty() && snapshot.commitText.empty() &&
                snapshot.candidates.empty(), "预期空组合与空提交；实际 " + describe(snapshot));
}

class DataLock {
public:
    explicit DataLock(const std::filesystem::path& root)
        : handle_(CreateFileW((root / L"XMJD.host.lock").c_str(), GENERIC_READ | GENERIC_WRITE,
                             0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_HIDDEN, nullptr)) {
        require(handle_ != INVALID_HANDLE_VALUE,
                "无法独占测试数据锁，Win32=" + std::to_string(GetLastError()));
    }
    ~DataLock() { CloseHandle(handle_); }
    DataLock(const DataLock&) = delete;
    DataLock& operator=(const DataLock&) = delete;
private:
    HANDLE handle_;
};

class TemporaryDirectory {
public:
    TemporaryDirectory() {
        const auto base = std::filesystem::temp_directory_path();
        for (unsigned attempt = 0; attempt != 8; ++attempt) {
            path_ = base / (L"XMJD-RimeRuntimeTests-" + std::to_wstring(GetCurrentProcessId()) +
                            L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
            std::error_code ec;
            if (std::filesystem::create_directory(path_, ec) && !ec) return;
        }
        throw std::runtime_error("无法创建隔离测试目录");
    }
    ~TemporaryDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

void testSettingsSchemaWrite() {
    TemporaryDirectory temporary;
    const auto settings = temporary.path() / L"settings.ini";
    {
        std::ofstream stream(settings, std::ios::binary);
        stream << "schema=xmjd6\n"
               << "scheme_source=F:/JD6\n"
               << "theme=dark\n";
    }

    require(xmjd::win::detail::writeSettingsSchemaFile(settings, "english"),
            "保留其他设置时写入 schema 失败");
    std::string contents;
    {
        std::ifstream updated(settings, std::ios::binary);
        contents.assign(std::istreambuf_iterator<char>(updated), std::istreambuf_iterator<char>());
    }
    require(contents == "schema=english\nscheme_source=F:/JD6\ntheme=dark\n",
            "写入 schema 覆盖或重复了其他设置：" + contents);

    {
        std::ofstream stream(settings, std::ios::binary | std::ios::trunc);
        stream << "scheme_source=F:/JD6\n";
    }
    require(xmjd::win::detail::writeSettingsSchemaFile(settings, "xmjd6"),
            "没有既有 schema 时追加设置失败");
    std::string appendedContents;
    {
        std::ifstream appended(settings, std::ios::binary);
        appendedContents.assign(std::istreambuf_iterator<char>(appended), std::istreambuf_iterator<char>());
    }
    require(appendedContents == "scheme_source=F:/JD6\nschema=xmjd6\n",
            "追加 schema 未保留方案来源：" + appendedContents);
    require(!xmjd::win::detail::writeSettingsSchemaFile(settings, "english/schema"),
            "非法 schema 标识被接受");
}

void testCorrection(RimeRuntime& runtime, const std::filesystem::path& root, std::string_view code, int strength = 35) {
    const auto corrected = runtime.createSession();
    require(runtime.correctionAvailable(corrected), "键道6 私有编译目录缺少纠错组件");
    auto prefs = xmjd::loadCorrectionPreferences(root);
    require(prefs.enabled && prefs.strength == 35 && prefs.leadTenths == 14, "纠错默认配置未与 Mac 对齐");
    if (strength != prefs.strength) {
        prefs.strength = strength;
        require(xmjd::saveCorrectionPreferences(root, prefs), "纠错强度无法保存");
    }
    const auto spell = [&](std::string_view input) {
        for (char c : input) {
            require(runtime.processKey(corrected, static_cast<unsigned char>(c), 0), "纠错测试码未被处理");
            (void)runtime.snapshot(corrected);
        }
    };
    runtime.setCorrectionContext(corrected, "下雨了，你有没有");
    const auto started = std::chrono::steady_clock::now();
    spell(code);
    const auto original = runtime.snapshot(corrected);
    require(original.correctionPending && !original.candidates.empty(), "键道候选未提交异步纠错；" + describe(original));
    std::optional<Snapshot> suggested;
    for (unsigned i = 0; i < 120; ++i) {
        suggested = runtime.refreshCandidates(corrected);
        if (!suggested || !suggested->correctionPending) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    require(suggested && !suggested->correctionPending && suggested->commitText.empty() &&
            !suggested->candidates.empty() && suggested->candidates.front().text == original.candidates.front().text,
            "纠错异步结果不可用、改动首选或自动上屏");
    std::cout << "MSIME candidate latency: " << std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count() << " ms" << std::endl;
    const auto correction = std::find_if(suggested->candidates.begin(), suggested->candidates.end(),
        [](const auto& candidate) { return candidate.text == "带伞" && candidate.comment.find("〔纠错〕") != candidate.comment.npos; });
    require(correction != suggested->candidates.end(), "Mac 示例未出现带伞纠错；" + describe(*suggested));
    require(runtime.selectCandidate(corrected, static_cast<std::uint32_t>(
        std::distance(suggested->candidates.begin(), correction))), "无法选择纠错候选");
    require(runtime.snapshot(corrected).commitText == "带伞", "纠错候选未正常上屏");
    prefs = xmjd::loadCorrectionPreferences(root);
    require(prefs.enabled && prefs.strength == strength, "纠错强度未保存");
    prefs.enabled = false;
    prefs.strength = 56;
    prefs.leadTenths = 20;
    require(xmjd::saveCorrectionPreferences(root, prefs), "纠错设置无法保存");
    prefs = xmjd::loadCorrectionPreferences(root);
    require(!prefs.enabled && prefs.strength == 56 && prefs.leadTenths == 20, "纠错设置无法读回");
    runtime.setCorrectionContext(corrected, "下雨了，你有没有");
    spell(code);
    require(!runtime.snapshot(corrected).correctionPending && !runtime.refreshCandidates(corrected),
            "纠错开关关闭后仍提交了纠错任务");
    runtime.cancel(corrected);
    runtime.destroySession(corrected);
}

void testSingleCharacterCorrection(RimeRuntime& runtime, const std::filesystem::path& root) {
    const auto session = runtime.createSession();
    require(runtime.correctionAvailable(session), "单字纠错缺少模型");
    constexpr auto context = "床前明月";
    constexpr auto code = "bm"; // 变；与「光」的 gm 相邻。
    auto prefs = xmjd::CorrectionPreferences{};
    auto hasCorrection = [](const Snapshot& snapshot) {
        return std::any_of(snapshot.candidates.begin(), snapshot.candidates.end(),
            [](const auto& candidate) { return candidate.comment.find("〔纠错〕") != candidate.comment.npos; });
    };
    const auto spell = [&](std::string_view input) {
        Snapshot snapshot;
        for (const unsigned char c : input) {
            require(runtime.processKey(session, c, 0), "单字测试码未处理");
            snapshot = runtime.snapshot(session);
        }
        return snapshot;
    };
    const auto clear = [&] {
        runtime.cancel(session);
        (void)runtime.snapshot(session);
    };
    const auto poll = [&] {
        for (unsigned i = 0; i < 120; ++i) {
            auto updated = runtime.refreshCandidates(session);
            if (!updated || !updated->correctionPending) return updated;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return std::optional<Snapshot>{};
    };
    // A +28 settings file has no single_character entry; two-key behavior stays unchanged.
    {
        std::ofstream legacy(root / L"correction.ini", std::ios::binary | std::ios::trunc);
        legacy << "enabled=1\nstrength=35\nlead_tenths=14\n";
    }
    require(!xmjd::loadCorrectionPreferences(root).singleCharacter, "旧设置错误开启单字纠错");
    runtime.setCorrectionContext(session, context);
    const auto baseline = spell(code);
    require(!baseline.correctionPending && !baseline.candidates.empty() && !hasCorrection(baseline) &&
            !runtime.refreshCandidates(session), "默认关闭的两码纠错干预输入");
    const auto first = baseline.candidates.front().text;
    const auto baselineContinued = spell("d");
    clear();

    prefs.singleCharacter = true;
    require(xmjd::saveCorrectionPreferences(root, prefs) &&
            xmjd::loadCorrectionPreferences(root).singleCharacter, "单字纠错开关未持久化");
    runtime.setCorrectionContext(session, context);
    const auto early = spell(code);
    require(early.correctionPending && !hasCorrection(early) && early.commitText.empty() &&
            !early.candidates.empty() && early.candidates.front().text == first, "第二键未保留普通候选");
    require(runtime.selectCandidate(session, 0) && runtime.snapshot(session).commitText == first,
            "停顿计时期间不能直接上屏首选");
    require(!runtime.refreshCandidates(session), "已上屏后仍有单字纠错结果");

    for (bool refresh : {false, true}) {
        runtime.setCorrectionContext(session, "今天我在这里测试怎么");
        const auto reading = spell("dj");
        require(reading.correctionPending && !reading.candidates.empty() &&
                reading.candidates.front().text == "读", "两码纠错改变了 dj 的普通首选");
        if (refresh) {
            const auto updated = poll();
            require(updated && updated->commitText.empty() && !updated->candidates.empty() &&
                    updated->candidates.front().text == "读", "候选刷新后 dj 丢失普通首选");
        }
        require(runtime.processKey(session, 0x20, 0) && runtime.snapshot(session).commitText == "读",
                "开启两码纠错后 dj 加空格没有上屏「读」");
        require(!runtime.refreshCandidates(session), "空格上屏以后仍有待处理的两码结果");
    }

    runtime.setCorrectionContext(session, context);
    require(spell(code).correctionPending, "两码未开始停顿计时");
    const auto continued = spell("d");
    require(continued.rawInput != code && !hasCorrection(continued) &&
            continued.rawInput == baselineContinued.rawInput &&
            continued.commitText == baselineContinued.commitText &&
            !runtime.refreshCandidates(session), "继续输入时应用了旧两码结果，或改变了原顶屏行为");
    clear();

    const auto suggestMoon = [&] {
        runtime.setCorrectionContext(session, "举头望明");
        const auto started = std::chrono::steady_clock::now();
        const auto original = spell("gh"); // 该；与「月」的 yh 相邻。
        const auto suggested = poll();
        require(suggested && suggested->rawInput == "gh" && suggested->commitText.empty() &&
                !original.candidates.empty() && suggested->candidates.size() >= 2 &&
                suggested->candidates[0].text == original.candidates[0].text &&
                suggested->candidates[1].text == "月" &&
                suggested->candidates[1].comment.find("〔纠错〕") != std::string::npos,
                "单字纠错没有作为次选出现、改变了首选或自动上屏；" + describe(suggested.value_or(Snapshot{})));
        require(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds(200),
                "两码停顿计时未生效");
    };
    suggestMoon();
    const auto afterSuggestion = spell("d");
    require(afterSuggestion.rawInput != "gh" && !hasCorrection(afterSuggestion) &&
            afterSuggestion.commitText != "月" && !runtime.refreshCandidates(session), "显示后继续输入未撤销旧纠错");
    clear();
    suggestMoon();
    require(runtime.selectCandidate(session, 1) && runtime.snapshot(session).commitText == "月", "两码纠错无法选择上屏");
    std::cout << "Single-character correction: gh suggested 月 after a pause, without changing the first candidate." << std::endl;
    runtime.setCorrectionContext(session, "一行白鹭上青");
    require(spell("gm").correctionPending, "单字低置信测试没有进入推理");
    const auto uncertain = poll();
    require(uncertain && !hasCorrection(*uncertain) && uncertain->commitText.empty(),
            "把握不足时仍提示了单字纠错");
    clear();
    runtime.setCorrectionContext(session, "");
    require(!spell(code).correctionPending && !runtime.refreshCandidates(session), "没有前文时仍启动两码纠错");
    runtime.destroySession(session);
}

void testExternalSource(const std::filesystem::path& hostDirectory) {
    const TemporaryDirectory temporary;
    const auto source = temporary.path() / L"selected scheme";
    const auto root = temporary.path() / L"app data";
    const auto write = [](const std::filesystem::path& path, std::string_view contents) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << contents;
        require(static_cast<bool>(stream), "不能创建外部方案测试文件");
    };
    write(root / "settings.ini", "schema=external_probe\nscheme_source=" + source.u8string() + "\n");
    const auto oldUser = root / "UserData/Rime";
    write(oldUser / "default.custom.yaml", "patch:\n  ascii_composer/switch_key/Shift_L: noop\n");
    write(oldUser / "lua/probe/main.lua", "error('must never use the old profile Lua')\n");
    write(source / "default.yaml",
        "config_version: '1'\nschema_list:\n  - schema: external_probe\n"
        "ascii_composer:\n  switch_key:\n    Shift_L: noop\n    Shift_R: noop\n");
    write(source / "default.custom.yaml", "patch:\n  ascii_composer/switch_key:\n    Shift_L: commit_code\n");
    write(source / "external_probe.schema.yaml",
        "schema:\n  schema_id: external_probe\n  name: External probe\n  version: '1'\n"
        "engine:\n  processors: [ascii_composer, 'lua_processor@*probe.main', speller, selector, express_editor]\n"
        "  segmentors: [abc_segmentor, fallback_segmentor]\n  translators: [table_translator]\n"
        "translator:\n  dictionary: external_probe\n");
    write(source / "external_probe.dict.yaml",
        "---\nname: external_probe\nversion: '1'\nsort: original\n...\n外\ta\n");
    write(source / "essay.txt", "外\t1\n");
    write(source / "build/default.yaml", "stale source cache must not be deployed\n");
    write(source / "opencc/probe.dat", "selected-data");
    write(source / "lua/probe/main.lua",
        "return function(key, env)\n"
        "  if key:repr() ~= 'F8' then return 2 end\n"
        "  local dir = rime_api.get_user_data_dir()\n"
        "  local f = assert(io.open(dir .. '/opencc/probe.dat', 'rb'))\n"
        "  local data = f:read('*a'); f:close()\n"
        "  env.engine:commit_text(require('probe.label') .. ':' .. data)\n"
        "  return 1\nend\n");
    write(source / "lua/probe/label.lua", "return 'selected-lua'\n");
    write(source / "lua/probe/removed.lua", "return 'stale'\n");
    write(source / "installation.yaml", "installation_id: source-must-not-change\n");
    const auto timestamp = std::filesystem::last_write_time(source / "installation.yaml");

    const DataLock lock(root);
    RimeRuntime runtime(hostDirectory, root);
    int error = 0;
    const bool initialized = runtime.initialize(false, error);
    require(initialized, "外部 Rime 源首次部署失败：" + std::to_string(error));
    const auto verify = [&](bool toggle, std::string_view luaResult) {
        const auto session = runtime.createSession();
        require(session != 0 && !runtime.snapshot(session).asciiMode, "外部会话未初始化为中文");
        require(runtime.processKey(session, 0xffc5, 0) &&
                runtime.snapshot(session).commitText == luaResult, "外部 Lua 或相对资源没有加载");
        require(runtime.processKey(session, 'a', 0), "外部词典编码没有处理");
        const auto word = runtime.snapshot(session);
        require(!word.candidates.empty() && word.candidates.front().text == "外", "外部词典没有加载");
        (void)runtime.processKey(session, xmjd::kLeftShiftKey, xmjd::kShiftTapMask);
        const auto shifted = runtime.snapshot(session);
        require(shifted.asciiMode == toggle && shifted.commitText == (toggle ? "a" : ""),
                "外部 default.custom.yaml 的 Shift 覆盖未生效；" + describe(shifted));
        runtime.destroySession(session);
    };
    verify(true, "selected-lua:selected-data");

    // Delete a source file and change a Lua dependency without modifying either
    // the built-in user profile or this source's base default.yaml.
    std::filesystem::remove(source / "lua/probe/removed.lua");
    write(source / "default.custom.yaml", "patch:\n  ascii_composer/switch_key/Shift_L: noop\n");
    write(source / "opencc/probe.dat", "new-data");
    write(source / "lua/probe/label.lua",
          "assert(not pcall(require, 'probe.removed')); return 'updated-lua'\n");
    const bool redeployed = runtime.redeploy(error);
    require(redeployed, "外部 Rime 源重新部署失败：" + std::to_string(error));
    verify(false, "updated-lua:new-data");
    require(std::filesystem::last_write_time(source / "installation.yaml") == timestamp &&
            !std::filesystem::exists(source / "user.yaml"), "部署写入了外部方案原目录");
    require(!std::filesystem::exists(root / "SharedData/Rime"), "外部模式发布了内置资源");

    write(root / "settings.ini", "schema=external_probe\nscheme_source=" + (source / "missing").u8string() + "\n");
    require(!runtime.redeploy(error) && error == 11 &&
            !std::filesystem::exists(root / "SharedData/Rime"), "无效外部目录悄悄回退到内置方案");
    std::cout << "External source: custom Shift, Lua and its data, stale-file removal, and no built-in fallback passed." << std::endl;
}

void testRuntime(const std::filesystem::path& hostDirectory, const std::filesystem::path& root,
                 std::string& stage) {
    const DataLock lock(root);
    RimeRuntime runtime(hostDirectory, root);
    require(!runtime.ready(), "初始化前不应 ready");
    stage = "首次部署打包方案及加载真实 librime/Lua";
    std::cout << stage << std::endl;
    int errorCode = 0;
    const bool initialized = runtime.initialize(false, errorCode);
    require(initialized, "initialize(false) 失败，runtime errorCode=" + std::to_string(errorCode));
    require(errorCode == 0 && runtime.ready(), "初始化成功后错误码或 ready 状态异常");
    const HMODULE library = GetModuleHandleW(L"rime.dll");
    require(library && std::filesystem::equivalent(xmjd::win::modulePath(library), hostDirectory / L"rime.dll"),
            "未加载 Host 输出目录中的真实 rime.dll");
    require(std::filesystem::is_regular_file(root / L"SharedData/Rime/.xmjd-resource-ready"),
            "首次部署未发布资源就绪标记");
    for (const auto* name : {L"pinyin_simp", L"liangfen", L"xmjd6.gbk"}) {
        require(std::filesystem::is_regular_file(root / L"Staging/Rime" /
                    (std::wstring(name) + L".table.bin")),
                "反查依赖词典未编译，检查 default.yaml 预设和 schema 依赖");
    }

    stage = "创建会话及验证默认中文模式";
    const auto session = runtime.createSession();
    require(session != 0, "createSession 失败");
    const auto initial = runtime.snapshot(session);
    require(!initial.asciiMode, "全新用户会话未默认使用中文模式");
    requireEmpty(initial);

    // 测试码来自锁定 zip 的 xmjd6.danzi/core 字典；F7 来自方案 key_binder。
    auto compose = [&](char key, const std::string& expectedFirst) {
        require(runtime.processKey(session, static_cast<unsigned char>(key), 0),
                std::string("processKey 未处理测试码 ") + key);
        const auto snapshot = runtime.snapshot(session);
        require(!snapshot.asciiMode && snapshot.rawInput == std::string(1, key) &&
                    !snapshot.preedit.empty() && snapshot.commitText.empty(),
                "输入测试码后组合状态异常；" + describe(snapshot));
        require(snapshot.page == 0 && snapshot.pageSize == 5 && !snapshot.candidates.empty() &&
                    snapshot.candidates.front().text == expectedFirst,
                "打包方案的首选或自定义候选页配置异常；预期首选=[" + expectedFirst + "]；" + describe(snapshot));
        for (const auto& candidate : snapshot.candidates) {
            require(!candidate.text.empty() && xmjd::validUtf8(candidate.text) && !candidate.label.empty(),
                    "候选文本或标签异常；" + describe(snapshot));
        }
    };

    unsigned deliveredCommits = 0;
    std::string deliveredText;
    auto consumeOnce = [&](const std::string& expected) {
        require(runtime.snapshot(session, false).commitText.empty(), "非消费快照不应返回提交文本");
        auto committed = runtime.snapshot(session);
        require(committed.commitText == expected,
                "上屏文本不符；预期=[" + expected + "]；" + describe(committed));
        ++deliveredCommits;
        deliveredText += committed.commitText;
        committed.commitText.clear();
        requireEmpty(committed);
        requireEmpty(runtime.snapshot(session));
        requireEmpty(runtime.snapshot(session));
        std::cout << stage << "：提交=[" << expected << "]，累计上屏次数=" << deliveredCommits << std::endl;
    };

    stage = "单键 n 生成中文候选及 selectCandidate(0) 一次性上屏";
    compose('n', "你");
    require(runtime.selectCandidate(session, 0), "selectCandidate(0) 失败");
    consumeOnce("你");

    stage = "finalize(convert=true) 转换后一次性上屏";
    compose('b', "不");
    runtime.finalize(session, true);
    consumeOnce("不");

    stage = "finalize(convert=false) 原始编码一次性上屏";
    compose('n', "你");
    runtime.finalize(session, false);
    consumeOnce("n");

    stage = "cancel 清空组合且不产生提交";
    compose('n', "你");
    runtime.cancel(session);
    requireEmpty(runtime.snapshot(session));
    requireEmpty(runtime.snapshot(session));
    compose('n', "你");
    runtime.finalize(session, true);
    runtime.cancel(session);
    requireEmpty(runtime.snapshot(session));
    requireEmpty(runtime.snapshot(session));

    stage = "F7 触发打包 Lua/OpenCC 数据的 个→個 转换";
    compose('g', "个");
    runtime.cancel(session);
    require(runtime.processKey(session, 0xffc4, 0), "F7 简繁切换未被处理");
    requireEmpty(runtime.snapshot(session));
    compose('g', "個");
    require(runtime.selectCandidate(session, 0), "繁体候选 selectCandidate(0) 失败");
    consumeOnce("個");
    require(runtime.processKey(session, 0xffc4, 0), "F7 切回简体未被处理");
    compose('g', "个");
    runtime.cancel(session);
    requireEmpty(runtime.snapshot(session));

    stage = "F4 方案选单及 Escape 返回中文输入";
    require(runtime.processKey(session, 0xffc1, 0), "F4 方案选单未被处理");
    const auto switcher = runtime.snapshot(session);
    require(switcher.commitText.empty() && !switcher.candidates.empty(),
            "F4 选单没有候选或产生了意外提交；" + describe(switcher));
    require(runtime.processKey(session, 0xff1b, 0), "Escape 未关闭方案选单");
    requireEmpty(runtime.snapshot(session));
    compose('n', "你");
    runtime.cancel(session);
    requireEmpty(runtime.snapshot(session));

    stage = "左右 Shift 单击保留方案的 commit_code 语义";
    require(!runtime.snapshot(session).asciiMode, "测试开始时应为中文模式");
    compose('n', "你");
    (void)runtime.processKey(session, xmjd::kLeftShiftKey, xmjd::kShiftTapMask);
    const auto shifted = runtime.snapshot(session);
    require(shifted.asciiMode && shifted.commitText == "n" && shifted.rawInput.empty(),
            "左 Shift 未切到英文并提交原始编码；" + describe(shifted));
    require(runtime.snapshot(session).commitText.empty(), "Shift 提交被重复读取");
    (void)runtime.processKey(session, xmjd::kRightShiftKey, xmjd::kShiftTapMask);
    require(!runtime.snapshot(session).asciiMode, "右 Shift 未切回中文");
    (void)runtime.processKey(session, 'n', xmjd::kShiftTapMask);
    requireEmpty(runtime.snapshot(session));

    stage = "销毁会话后拒绝旧会话及校验上屏总次数";
    runtime.destroySession(session);
    require(!runtime.processKey(session, 'n', 0), "已销毁会话仍可处理按键");
    require(!runtime.selectCandidate(session, 0), "已销毁会话仍可选择候选");
    requireEmpty(runtime.snapshot(session));
    require(deliveredCommits == 4 && deliveredText == "你不n個", "上屏次数或汇总文本异常");

    stage = "使用 Mac 原模型修正邻键误按";
    // The bundled dictionary has 岱山 at dhef; the Mac demo uses a newer dictionary.
    testCorrection(runtime, root, "dhef");
    stage = "两码单字：默认关闭、等待期间可上屏、第三键撤回和停顿后提示";
    testSingleCharacterCorrection(runtime, root);

    stage = "切换拼音方案、重新部署并验证真实拼音候选";
    {
        std::ofstream settings(root / L"settings.ini", std::ios::binary | std::ios::trunc);
        settings << "schema=pinyin_simp\n";
        std::ofstream custom(root / L"UserData/Rime/default.custom.yaml", std::ios::binary | std::ios::trunc);
        custom << "patch:\n  schema_list:\n    - schema: pinyin_simp\n    - schema: xmjd6\n    - schema: liangfen\n";
    }
    int redeployError = 0;
    require(runtime.redeploy(redeployError),
            "切换到拼音方案重新部署失败，runtime errorCode=" + std::to_string(redeployError));
    const auto pinyinSession = runtime.createSession();
    require(pinyinSession != 0, "拼音方案会话创建失败");
    require(!runtime.correctionAvailable(pinyinSession), "非键道方案错误启用纠错");
    for (const char key : std::string("nihao")) {
        require(runtime.processKey(pinyinSession, static_cast<unsigned char>(key), 0),
                std::string("拼音方案未处理按键 ") + key);
    }
    const auto pinyinSnapshot = runtime.snapshot(pinyinSession);
    require(!pinyinSnapshot.rawInput.empty() && !pinyinSnapshot.preedit.empty() &&
                !pinyinSnapshot.candidates.empty(),
            "拼音方案未生成候选；" + describe(pinyinSnapshot));
    const auto nihao = std::find_if(pinyinSnapshot.candidates.begin(), pinyinSnapshot.candidates.end(),
        [](const auto& candidate) { return candidate.text == "你好"; });
    require(nihao != pinyinSnapshot.candidates.end(), "拼音方案候选缺少 你好；" + describe(pinyinSnapshot));
    const auto nihaoIndex = static_cast<std::uint32_t>(
        std::distance(pinyinSnapshot.candidates.begin(), nihao));
    require(runtime.selectCandidate(pinyinSession, nihaoIndex), "拼音方案选择 你好 失败");
    const auto pinyinCommit = runtime.snapshot(pinyinSession);
    require(pinyinCommit.commitText == "你好", "拼音方案上屏文本异常；" + describe(pinyinCommit));
    runtime.destroySession(pinyinSession);
    std::cout << "RimeRuntimeTests 通过：上屏次数=4，文本=[" << deliveredText
              << "]，拼音切换提交=[" << pinyinCommit.commitText << "]" << std::endl;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc == 2 && _wcsicmp(argv[1], L"--settings-schema") == 0) {
        try {
            testSettingsSchemaWrite();
            std::cout << "RimeRuntimeTests 设置写入回归通过" << std::endl;
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "RimeRuntimeTests 设置写入回归失败：" << error.what() << '\n';
            return 1;
        }
    }
    std::string stage = "校验构建资源并创建隔离测试目录";
    try {
        require(argc == 2 || argc == 3, "用法：RimeRuntimeTests <XMJDHost 输出目录> [外部 JD6 测试目录]");
        const std::filesystem::path hostDirectory(argv[1]);
        require(hostDirectory.is_absolute() && std::filesystem::is_directory(hostDirectory),
                "Host 输出目录必须是存在的绝对路径");
        require(std::filesystem::is_regular_file(hostDirectory / L"rime.dll") &&
                    std::filesystem::is_regular_file(hostDirectory / L"Resources/Rime/xmjd6.schema.yaml"),
                "Host 打包输出缺少 rime.dll 或方案资源，请先构建 XMJDHost");
        const TemporaryDirectory temporary;
        if (argc == 3) {
            const std::filesystem::path external(argv[2]);
            require(external.is_absolute() && std::filesystem::is_regular_file(external / L"default.yaml"),
                    "外部测试方案不存在");
            const auto before = std::filesystem::last_write_time(external / L"xmjd6.schema.yaml");
            const auto u8 = external.u8string();
            {
                std::ofstream settings(temporary.path() / L"settings.ini", std::ios::binary);
                settings << "schema=xmjd6\nscheme_source=" << u8 << '\n';
            }
            const DataLock lock(temporary.path());
            RimeRuntime runtime(hostDirectory, temporary.path());
            int error = 0;
            const bool initialized = runtime.initialize(false, error);
            require(initialized, "外部测试方案部署失败: " + std::to_string(error));
            stage = "外部 JD6：验证 msc 首选、上屏，以及来自源目录的 Shift 补丁";
            const auto session = runtime.createSession();
            require(session != 0, "无法创建 JD6 会话");
            for (unsigned char key : std::string_view("msc")) {
                require(runtime.processKey(session, key, 0), "JD6 未处理 msc");
                (void)runtime.snapshot(session);
            }
            const auto choices = runtime.snapshot(session);
            // JD6's eng_space option may pad English candidates on both sides.
            const auto isMac = [](std::string_view text) {
                const auto first = text.find_first_not_of(' ');
                return first != text.npos && text.substr(first, text.find_last_not_of(' ') - first + 1) == "MAC";
            };
            require(!choices.candidates.empty() && isMac(choices.candidates.front().text),
                    "JD6 的 msc 首选不是 MAC；" + describe(choices));
            require(runtime.processKey(session, ' ', 0) && isMac(runtime.snapshot(session).commitText),
                    "JD6 的 msc 加空格未上屏 MAC");
            (void)runtime.processKey(session, 'n', 0);
            (void)runtime.snapshot(session);
            (void)runtime.processKey(session, xmjd::kLeftShiftKey, xmjd::kShiftTapMask);
            const auto shift = runtime.snapshot(session);
            require(shift.asciiMode && shift.commitText == "n", "JD6 的左 Shift 没有提交编码并切换英文");
            runtime.destroySession(session);
            std::cout << "JD6: msc committed MAC and Left Shift committed the code and switched to ASCII." << std::endl;
            stage = "外部 JD6：按当前词库将纠错强度调到 90 后测试 Mac 的 dhsd 示例";
            testCorrection(runtime, temporary.path(), "dhsd", 90);
            require(std::filesystem::last_write_time(external / L"xmjd6.schema.yaml") == before,
                    "外部方案的 schema 不应被修改");
        } else {
            testRuntime(hostDirectory, temporary.path(), stage);
            stage = "隔离外部 Rime 目录、自定义补丁和 Lua 的重新部署";
            testExternalSource(hostDirectory);
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RimeRuntimeTests 失败，阶段=[" << stage << "]：" << error.what() << '\n';
        return 1;
    }
}
