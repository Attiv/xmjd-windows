#include "ContextualCorrection.h"
#include "msime_scorer.h"
#include "../../Protocol/Protocol.h"

#include <windows.h>
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <optional>
#include <regex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
namespace xmjd::correction {
namespace {
constexpr std::string_view kPrefix = "xmjd_win_msime/";

std::string read(const fs::path& path, std::size_t max) {
    std::ifstream f(path, std::ios::binary);
    std::string s(max + 1, '\0');
    f.read(s.data(), s.size());
    s.resize(static_cast<std::size_t>(f.gcount()));
    return s.size() <= max ? s : std::string{};
}

bool plain(const fs::path& path) {
    const DWORD flags = GetFileAttributesW(path.c_str());
    return flags != INVALID_FILE_ATTRIBUTES && !(flags & FILE_ATTRIBUTE_REPARSE_POINT);
}

bool update(const fs::path& target, const std::string& bytes) {
    if (bytes.empty()) return false;
    if (read(target, bytes.size()) == bytes) return plain(target);
    auto tmp = target;
    tmp += L".xmjd-msime.tmp";
    if (GetFileAttributesW(tmp.c_str()) != INVALID_FILE_ATTRIBUTES && !plain(tmp)) return false;
    std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
    file.write(bytes.data(), bytes.size());
    file.close();
    const bool ok = file && MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

bool schemaId(std::string_view s) {
    return !s.empty() && s.size() <= 80 && s != "." && s != ".." && s.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") == s.npos;
}

std::string capture(const std::string& s, const std::regex& pattern) {
    std::smatch m;
    return std::regex_search(s, m, pattern) ? m[1].str() : std::string{};
}

bool jiandao(const std::string& s, const std::string& id, const fs::path& shared, const fs::path& user) {
    if (id == "xmjd6") return true;
    const auto begin = s.find("\ntranslator:\n");
    if (begin == s.npos) return false;
    const auto end = s.find("\n", begin + 13);
    auto cursor = end;
    while (cursor != s.npos && cursor + 1 < s.size() && s[cursor + 1] == ' ')
        cursor = s.find('\n', cursor + 1);
    const auto block = s.substr(begin, cursor == s.npos ? s.npos : cursor - begin);
    static const std::regex dictionary(R"(\n  dictionary: ["']?([a-zA-Z0-9_.-]+))");
    const auto dict = capture(block, dictionary);
    if (!schemaId(dict) || s.find("勿选") != s.npos) return false;
    for (const auto& folder : {user, shared}) {
        std::ifstream file(folder / (dict + ".dict.yaml"), std::ios::binary);
        std::string header(65'536, '\0');
        file.read(header.data(), header.size());
        header.resize(static_cast<std::size_t>(file.gcount()));
        const auto marker = header.find("\n...");
        const auto rule = header.find("AaAbBaBb");
        if (rule != header.npos && (marker == header.npos || rule < marker)) return true;
        if (!header.empty()) return false;
    }
    return false;
}

bool hook(std::string& text, std::string_view section, std::string_view anchor,
          std::string_view kind, std::string_view script) {
    const auto start = text.find("\n  " + std::string(section) + ":\n");
    if (start == text.npos) return false;
    auto next = text.find('\n', start + 1);
    const auto first = next + 1;
    const std::string line = "    - \"" + std::string(kind) + "@*" + std::string(kPrefix) + std::string(script) + "\"\n";
    std::size_t insert = anchor.empty() ? first : text.npos;
    while (next != text.npos && text.compare(next + 1, 6, "    - ") == 0) {
        const auto lineEnd = text.find('\n', next + 1);
        const auto row = text.substr(next + 1, lineEnd == text.npos ? text.npos : lineEnd - next - 1);
        // A schema that already has the shared upstream hook needs no duplicate.
        if (row.find(script) != row.npos) return true;
        if (insert == text.npos && (row == "    - " + std::string(anchor) ||
            row == "    - \"" + std::string(anchor) + "\"")) insert = lineEnd + 1;
        next = lineEnd;
    }
    if (insert == text.npos) return false;
    text.insert(insert, line);
    return true;
}

std::vector<std::string_view> split(std::string_view text, char delimiter) {
    std::vector<std::string_view> fields;
    for (;;) {
        const auto pos = text.find(delimiter);
        fields.push_back(text.substr(0, pos));
        if (pos == text.npos) return fields;
        text.remove_prefix(pos + 1);
    }
}

std::vector<char32_t> scalars(std::string_view text) {
    const auto u = utf8ToUtf16(text);
    std::vector<char32_t> result;
    for (std::size_t i = 0; i < u.size(); ++i) {
        char32_t c = u[i];
        if (c >= 0xd800 && c <= 0xdbff && i + 1 < u.size())
            c = 0x10000 + ((c - 0xd800) << 10) + (u[++i] - 0xdc00);
        result.push_back(c);
    }
    return result;
}

std::string format(float v) {
    char buf[32]{};
    const auto out = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::general);
    return out.ec == std::errc{} ? std::string(buf, out.ptr) : std::string{};
}
} // namespace

bool validCode(std::string_view code) noexcept {
    return (code.size() == 2 || (code.size() >= 4 && code.size() <= 6)) &&
           code.find_first_not_of("abcdefghijklmnopqrstuvwxyz") == code.npos;
}

std::string trailingContext(std::string_view text) {
    if (!validUtf8(text)) return {};
    std::size_t line = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (static_cast<unsigned char>(text[i]) < 0x20) line = i + 1;
    text.remove_prefix(line);
    std::size_t start = text.size();
    for (unsigned length = 0; start > 0 && length < 64; ++length) {
        do { --start; } while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xc0) == 0x80);
    }
    return std::string(text.substr(start));
}

std::unordered_set<std::string> installHooks(const fs::path& compiled, const fs::path& shared,
    const fs::path& user, const fs::path& resources) {
    const auto generated = user / "lua" / "xmjd_win_msime";
    std::error_code error;
    fs::create_directories(generated, error);
    if (error || !plain(compiled) || !plain(user / "lua") || !plain(generated)) return {};
    for (const auto* script : {"xmjd6_contextual_guard", "xmjd6_contextual_correction", "xmjd6_contextual_ranker"}) {
        const auto name = std::string(script) + ".lua";
        const auto contents = read(resources / "lua" / name, 65'536);
        if (!update(generated / name, contents)) return {};
    }
    std::unordered_set<std::string> enabled;
    constexpr std::string_view suffix = ".schema.yaml";
    for (fs::directory_iterator it(compiled, error), end; it != end && !error; it.increment(error)) {
        if (!it->is_regular_file(error) || !plain(it->path())) continue;
        const auto name = it->path().filename().string();
        if (name.size() < suffix.size() || std::string_view(name).substr(name.size() - suffix.size()) != suffix) continue;
        const auto id = name.substr(0, name.size() - suffix.size());
        if (!schemaId(id)) continue;
        auto source = read(it->path(), 512 * 1024);
        source.erase(std::remove(source.begin(), source.end(), '\r'), source.end());
        if (!jiandao(source, id, shared, user) ||
            !hook(source, "processors", "", "lua_processor", "xmjd6_contextual_guard") ||
            !hook(source, "translators", "table_translator", "lua_translator", "xmjd6_contextual_correction") ||
            !hook(source, "filters", "uniquifier", "lua_filter", "xmjd6_contextual_ranker")) continue;
        if (update(it->path(), source)) enabled.emplace(id);
    }
    return enabled;
}

struct Worker::Impl {
    struct Job { std::string context, pool; std::promise<std::string> result; };
    explicit Impl(fs::path path) : resources(std::move(path)), thread([this] { run(); }) {}
    ~Impl() {
        { std::lock_guard<std::mutex> lock(mutex); stop = true; pending.reset(); }
        ready.notify_one();
        thread.join();
    }
    std::future<std::string> submit(std::string context, std::string pool) {
        Job job{std::move(context), std::move(pool), {}};
        auto result = job.result.get_future();
        { std::lock_guard<std::mutex> lock(mutex); pending = std::move(job); }
        ready.notify_one();
        return result;
    }
    void run() {
        std::unique_lock<std::mutex> lock(mutex);
        while (true) {
            ready.wait(lock, [&] { return stop || pending; });
            if (stop) return;
            // Coalesce rapid edits before doing any inference.
            while (!stop && ready.wait_for(lock, std::chrono::milliseconds(25)) == std::cv_status::no_timeout) {}
            if (stop) return;
            auto job = std::move(*pending);
            pending.reset();
            lock.unlock();
            try { job.result.set_value(score(job.context, job.pool)); }
            catch (...) {
                failed = true;
                OutputDebugStringW(L"XMJD MSIME unavailable; continued ordinary Rime input.\n");
                job.result.set_value({});
            }
            lock.lock();
        }
    }
    std::string score(const std::string& context, const std::string& pool) {
        if (failed || context.empty() || pool.size() > 16'000 || !validUtf8(pool)) return {};
        auto lines = split(pool, '\n');
        if (lines.size() < 2 || lines.size() > 97) return {};
        const auto head = split(lines.front(), '\t');
        if (head.size() != 3 || head[0] != "msime2" || !validCode(head[2])) return {};
        if (!scorer) {
            std::ifstream file(resources / "vocab.tsv", std::ios::binary);
            unsigned cp = 0, idx = 0;
            while (file >> cp >> idx) {
                if (idx >= 12288 || cp > 0x10ffff || ids.size() >= 12288)
                    throw std::runtime_error("MSIME invalid vocabulary");
                ids.emplace(cp, idx);
            }
            if (ids.size() < 12000) throw std::runtime_error("MSIME vocabulary missing");
            scorer = std::make_unique<msime::Scorer>(resources / "model.ort", resources / "onnxruntime.dll");
        }
        std::vector<int32_t> prefix{2};
        for (auto c : scalars(context)) {
            const auto id = ids.find(c);
            prefix.push_back(id == ids.end() ? 1 : id->second);
        }
        if (prefix.size() <= 1 || prefix.size() > 65) return {};
        std::string output(lines.front());
        std::vector<std::vector<int32_t>> bodies;
        std::vector<std::string_view> rows;
        const auto flush = [&]() {
            if (rows.empty()) return;
            const auto prior = scorer->Score({2}, bodies);
            const auto contextual = scorer->Score(prefix, bodies);
            for (std::size_t i = 0; i < rows.size(); ++i) {
                output += '\n'; output.append(rows[i]); output += '\t'; output += format(contextual[i]);
                output += '\t'; output += format(contextual[i] - prior[i]);
            }
            rows.clear(); bodies.clear();
        };
        for (std::size_t i = 1; i < lines.size(); ++i) {
            const auto fields = split(lines[i], '\t');
            if (fields.size() != 3 || (fields[0] != "L" && fields[0] != "C") ||
                (i == 1 && fields[0] != "L") || fields[2].size() != head[2].size() ||
                !validCode(fields[2])) return {};
            auto codes = scalars(fields[1]);
            if (head[2].size() == 2 && codes.size() != 1) return {};
            std::vector<int32_t> body;
            for (auto c : codes) {
                if (c < 0x20 || (c >= 0x7f && c <= 0x9f)) return {};
                const auto id = ids.find(c);
                if (id == ids.end()) { body.clear(); break; }
                body.push_back(id->second);
            }
            if (body.empty() || body.size() > 16) { if (i == 1) return {}; else continue; }
            rows.push_back(lines[i]); bodies.push_back(std::move(body));
            if (rows.size() == 32) flush();
        }
        flush();
        return output;
    }
    fs::path resources;
    std::mutex mutex;
    std::condition_variable ready;
    bool stop = false;
    std::optional<Job> pending;
    std::unordered_map<char32_t, int32_t> ids;
    std::unique_ptr<msime::Scorer> scorer;
    bool failed = false;
    std::thread thread;
};
Worker::Worker(fs::path resources) : impl_(std::make_unique<Impl>(std::move(resources))) {}
Worker::~Worker() = default;
std::future<std::string> Worker::submit(std::string context, std::string pool) {
    return impl_->submit(std::move(context), std::move(pool));
}

} // namespace xmjd::correction
