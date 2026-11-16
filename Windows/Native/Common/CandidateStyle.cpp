#include "CandidateStyle.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace xmjd {
namespace {

constexpr std::size_t kMaxStyleBytes = 256 * 1024;
constexpr std::size_t kMaxCustomStyles = 128;
constexpr std::size_t kMaxJsonDepth = 24;

struct JsonValue {
    enum class Type { Null, Bool, Number, String, Object, Array } type = Type::Null;
    bool boolean = false;
    double number = 0;
    std::string text;
    std::vector<std::pair<std::string, JsonValue>> members;
    std::vector<JsonValue> elements;

    const JsonValue* find(std::string_view key) const {
        for (const auto& member : members) {
            if (member.first == key) return &member.second;
        }
        return nullptr;
    }
};

bool validUtf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const unsigned char lead = static_cast<unsigned char>(text[index]);
        std::size_t extra = 0;
        if (lead < 0x80) extra = 0;
        else if ((lead & 0xE0) == 0xC0) extra = 1;
        else if ((lead & 0xF0) == 0xE0) extra = 2;
        else if ((lead & 0xF8) == 0xF0) extra = 3;
        else return false;
        if (index + extra >= text.size()) return false;
        for (std::size_t offset = 1; offset <= extra; ++offset) {
            if ((static_cast<unsigned char>(text[index + offset]) & 0xC0) != 0x80) return false;
        }
        index += extra + 1;
    }
    return true;
}

void appendUtf8(std::string& out, unsigned int codePoint) {
    if (codePoint <= 0x7F) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

class JsonParser {
public:
    explicit JsonParser(std::string_view text) : text_(text) {}

    bool parse(JsonValue& value, std::string& error) {
        skipSpace();
        if (!parseValue(value, 0)) {
            error = error_.empty() ? "candidate style JSON is not valid" : error_;
            return false;
        }
        skipSpace();
        if (position_ != text_.size()) {
            error = "candidate style JSON has trailing content";
            return false;
        }
        return true;
    }

private:
    bool fail(const char* message) {
        if (error_.empty()) error_ = message;
        return false;
    }

    void skipSpace() {
        while (position_ < text_.size()) {
            const char current = text_[position_];
            if (current == ' ' || current == '\t' || current == '\r' || current == '\n') ++position_;
            else break;
        }
    }

    bool parseValue(JsonValue& value, std::size_t depth) {
        if (depth > kMaxJsonDepth) return fail("candidate style JSON is nested too deeply");
        skipSpace();
        if (position_ >= text_.size()) return fail("candidate style JSON ended early");
        switch (text_[position_]) {
        case '{': return parseObject(value, depth);
        case '[': return parseArray(value, depth);
        case '"': {
            value.type = JsonValue::Type::String;
            return parseString(value.text);
        }
        case 't':
            if (text_.compare(position_, 4, "true") != 0) return fail("invalid JSON literal");
            position_ += 4;
            value.type = JsonValue::Type::Bool;
            value.boolean = true;
            return true;
        case 'f':
            if (text_.compare(position_, 5, "false") != 0) return fail("invalid JSON literal");
            position_ += 5;
            value.type = JsonValue::Type::Bool;
            value.boolean = false;
            return true;
        case 'n':
            if (text_.compare(position_, 4, "null") != 0) return fail("invalid JSON literal");
            position_ += 4;
            value.type = JsonValue::Type::Null;
            return true;
        default: return parseNumber(value);
        }
    }

    bool parseObject(JsonValue& value, std::size_t depth) {
        value.type = JsonValue::Type::Object;
        ++position_;
        skipSpace();
        if (position_ < text_.size() && text_[position_] == '}') {
            ++position_;
            return true;
        }
        for (;;) {
            skipSpace();
            std::string key;
            if (!parseString(key)) return false;
            skipSpace();
            if (position_ >= text_.size() || text_[position_] != ':') return fail("missing ':' in candidate style");
            ++position_;
            JsonValue member;
            if (!parseValue(member, depth + 1)) return false;
            value.members.emplace_back(std::move(key), std::move(member));
            skipSpace();
            if (position_ >= text_.size()) return fail("candidate style JSON ended early");
            if (text_[position_] == ',') { ++position_; continue; }
            if (text_[position_] == '}') { ++position_; return true; }
            return fail("invalid object separator");
        }
    }

    bool parseArray(JsonValue& value, std::size_t depth) {
        value.type = JsonValue::Type::Array;
        ++position_;
        skipSpace();
        if (position_ < text_.size() && text_[position_] == ']') {
            ++position_;
            return true;
        }
        for (;;) {
            JsonValue element;
            if (!parseValue(element, depth + 1)) return false;
            value.elements.push_back(std::move(element));
            skipSpace();
            if (position_ >= text_.size()) return fail("candidate style JSON ended early");
            if (text_[position_] == ',') { ++position_; continue; }
            if (text_[position_] == ']') { ++position_; return true; }
            return fail("invalid array separator");
        }
    }

    bool parseString(std::string& out) {
        if (position_ >= text_.size() || text_[position_] != '"') return fail("expected a JSON string");
        ++position_;
        out.clear();
        while (position_ < text_.size()) {
            const char current = text_[position_++];
            if (current == '"') return true;
            if (current != '\\') {
                if (static_cast<unsigned char>(current) < 0x20) return fail("control character in JSON string");
                out.push_back(current);
                continue;
            }
            if (position_ >= text_.size()) return fail("truncated JSON escape");
            const char escape = text_[position_++];
            switch (escape) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                unsigned int codePoint = 0;
                if (!parseHex4(codePoint)) return false;
                if (codePoint >= 0xD800 && codePoint <= 0xDBFF && position_ + 6 <= text_.size() &&
                    text_[position_] == '\\' && text_[position_ + 1] == 'u') {
                    position_ += 2;
                    unsigned int low = 0;
                    if (!parseHex4(low)) return false;
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    } else {
                        return fail("invalid surrogate pair");
                    }
                }
                appendUtf8(out, codePoint);
                break;
            }
            default: return fail("unsupported JSON escape");
            }
        }
        return fail("unterminated JSON string");
    }

    bool parseHex4(unsigned int& value) {
        if (position_ + 4 > text_.size()) return fail("truncated \\u escape");
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const char digit = text_[position_++];
            value <<= 4;
            if (digit >= '0' && digit <= '9') value |= static_cast<unsigned int>(digit - '0');
            else if (digit >= 'a' && digit <= 'f') value |= static_cast<unsigned int>(digit - 'a' + 10);
            else if (digit >= 'A' && digit <= 'F') value |= static_cast<unsigned int>(digit - 'A' + 10);
            else return fail("invalid \\u escape");
        }
        return true;
    }

    bool parseNumber(JsonValue& value) {
        const std::size_t start = position_;
        if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) ++position_;
        bool digits = false;
        while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
            digits = true;
            ++position_;
        }
        if (position_ < text_.size() && text_[position_] == '.') {
            ++position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                digits = true;
                ++position_;
            }
        }
        if (!digits) return fail("invalid JSON number");
        int explicitExponent = 0;
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            const std::size_t marker = position_;
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '-' || text_[position_] == '+')) ++position_;
            bool exponentDigits = false;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') {
                exponentDigits = true;
                ++position_;
            }
            if (!exponentDigits) return fail("invalid JSON exponent");
            explicitExponent = std::atoi(std::string(text_.substr(marker + 1, position_ - marker - 1)).c_str());
        }
        // Locale-independent conversion: manual accumulation avoids strtod's
        // dependence on the C numeric locale.
        double sign = 1;
        std::size_t index = start;
        if (text_[index] == '-') { sign = -1; ++index; }
        else if (text_[index] == '+') { ++index; }
        double mantissa = 0;
        bool inFraction = false;
        int fractionDigits = 0;
        const std::size_t end = position_;
        for (; index < end; ++index) {
            const char current = text_[index];
            if (current == '.') { inFraction = true; continue; }
            if (current == 'e' || current == 'E') break;
            mantissa = mantissa * 10 + (current - '0');
            if (inFraction) ++fractionDigits;
        }
        double result = mantissa;
        for (int step = 0; step < fractionDigits; ++step) result /= 10;
        if (explicitExponent > 0) {
            for (int step = 0; step < explicitExponent; ++step) result *= 10;
        } else if (explicitExponent < 0) {
            for (int step = 0; step < -explicitExponent; ++step) result /= 10;
        }
        value.type = JsonValue::Type::Number;
        value.number = sign * result;
        return true;
    }

    std::string_view text_;
    std::size_t position_ = 0;
    std::string error_;
};

std::string formatNumber(double value) {
    char buffer[64]{};
    std::snprintf(buffer, sizeof(buffer), "%.10g", value);
    for (char* cursor = buffer; *cursor; ++cursor) {
        if (*cursor == ',') *cursor = '.';
    }
    return buffer;
}

void appendJsonString(std::string& out, std::string_view value) {
    out.push_back('"');
    for (const char current : value) {
        switch (current) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (static_cast<unsigned char>(current) < 0x20) {
                char escape[8]{};
                std::snprintf(escape, sizeof(escape), "\\u%04x", static_cast<unsigned char>(current));
                out += escape;
            } else {
                out.push_back(current);
            }
        }
    }
    out.push_back('"');
}

StyleColor makeColor(unsigned int rgb, double alpha = 1.0) {
    StyleColor color;
    color.red = static_cast<double>((rgb >> 16) & 0xFF) / 255.0;
    color.green = static_cast<double>((rgb >> 8) & 0xFF) / 255.0;
    color.blue = static_cast<double>(rgb & 0xFF) / 255.0;
    color.alpha = alpha;
    return color;
}

ColorPair makePair(unsigned int light, unsigned int dark, double lightAlpha = 1.0, double darkAlpha = 1.0) {
    return ColorPair{makeColor(light, lightAlpha), makeColor(dark, darkAlpha)};
}

// Document defaults mirror the Mac decoder (MacCandidateStyle property values).
CandidateStyle documentDefaults() {
    CandidateStyle style;
    style.horizontal = false;
    style.fontName.clear();
    style.fontSize = 17;
    style.commentSize = 13;
    style.rowHeight = 28;
    style.padding = 12;
    style.itemSpacing = 4;
    style.preeditSpacing = 6;
    style.cornerRadius = 9;
    style.itemRadius = 4;
    style.opacity = 1;
    style.maxWidth = 600;
    style.showLabels = true;
    style.showComments = true;
    style.showPreedit = true;
    style.background = makePair(0xF7F7FA, 0x24242A, 0.98, 0.98);
    style.text = makePair(0x292B30, 0xEBEDF2);
    style.accent = makePair(0x0063F0, 0x59A8FF);
    style.decoration.reset();
    return style;
}

bool readStyleFile(const std::filesystem::path& path, std::string& contents) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size > kMaxStyleBytes) return false;
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    contents.assign(static_cast<std::size_t>(size), '\0');
    if (!contents.empty()) {
        stream.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream || stream.gcount() != static_cast<std::streamsize>(contents.size())) return false;
    }
    return true;
}

bool readBool(const JsonValue& object, std::string_view key, bool& target) {
    const JsonValue* value = object.find(key);
    if (!value || value->type != JsonValue::Type::Bool) return false;
    target = value->boolean;
    return true;
}

bool readNumber(const JsonValue& object, std::string_view key, double& target) {
    const JsonValue* value = object.find(key);
    if (!value || value->type != JsonValue::Type::Number || !std::isfinite(value->number)) return false;
    target = value->number;
    return true;
}

bool readOptionalNumber(const JsonValue& object, std::string_view key, std::optional<double>& target) {
    const JsonValue* value = object.find(key);
    if (!value) return false;
    if (value->type == JsonValue::Type::Null) {
        target.reset();
        return true;
    }
    if (value->type != JsonValue::Type::Number || !std::isfinite(value->number)) return false;
    target = value->number;
    return true;
}

bool readString(const JsonValue& object, std::string_view key, std::string& target) {
    const JsonValue* value = object.find(key);
    if (!value || value->type != JsonValue::Type::String) return false;
    target = value->text;
    return true;
}

bool readColor(const JsonValue& value, StyleColor& color) {
    if (value.type != JsonValue::Type::Object) return false;
    double red = 0, green = 0, blue = 0, alpha = 1;
    if (!readNumber(value, "red", red) || !readNumber(value, "green", green) || !readNumber(value, "blue", blue)) {
        return false;
    }
    readNumber(value, "alpha", alpha);
    color = StyleColor{red, green, blue, alpha};
    return true;
}

bool readPair(const JsonValue* value, ColorPair& pair) {
    if (!value || value->type != JsonValue::Type::Object) return false;
    const JsonValue* light = value->find("light");
    const JsonValue* dark = value->find("dark");
    if (!light || !dark) return false;
    return readColor(*light, pair.light) && readColor(*dark, pair.dark);
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

bool colorInRange(const StyleColor& color) {
    for (const double channel : {color.red, color.green, color.blue, color.alpha}) {
        if (!std::isfinite(channel) || channel < 0 || channel > 1) return false;
    }
    return true;
}

struct JsonEmitter {
    bool pretty = true;
    std::string out;
    std::vector<bool> levelFirst{true};

    std::string indent() const { return std::string((levelFirst.size() - 1) * 2, ' '); }

    void comma() {
        if (!levelFirst.back()) out += ",";
        levelFirst.back() = false;
        if (pretty) out += "\n" + indent();
    }
    void beginObject() {
        out += "{";
        levelFirst.push_back(true);
    }
    void endObject() {
        const bool empty = levelFirst.back();
        levelFirst.pop_back();
        if (pretty && !empty) out += "\n" + indent();
        out += "}";
    }
    void beginMember(std::string_view name) {
        comma();
        appendJsonString(out, name);
        out += pretty ? ": " : ":";
    }
    void numberField(std::string_view name, double value) {
        beginMember(name);
        out += formatNumber(value);
    }
    void boolField(std::string_view name, bool value) {
        beginMember(name);
        out += value ? "true" : "false";
    }
    void stringField(std::string_view name, std::string_view value) {
        beginMember(name);
        appendJsonString(out, value);
    }
    void color(std::string_view name, const StyleColor& value) {
        beginMember(name);
        beginObject();
        numberField("red", value.red);
        numberField("green", value.green);
        numberField("blue", value.blue);
        numberField("alpha", value.alpha);
        endObject();
    }
    void pair(std::string_view name, const ColorPair& value) {
        beginMember(name);
        beginObject();
        color("light", value.light);
        color("dark", value.dark);
        endObject();
    }
};

} // namespace

std::string serializeCandidateStyle(const CandidateStyle& style, bool pretty) {
    JsonEmitter emitter;
    emitter.pretty = pretty;
    emitter.beginObject();
    emitter.stringField("format", style.format);
    emitter.numberField("version", style.version);
    emitter.stringField("id", style.id);
    emitter.stringField("name", style.name);
    emitter.stringField("layout", style.horizontal ? "horizontal" : "vertical");
    emitter.stringField("fontName", style.fontName);
    emitter.numberField("fontSize", style.fontSize);
    emitter.numberField("commentSize", style.commentSize);
    emitter.numberField("rowHeight", style.rowHeight);
    emitter.numberField("padding", style.padding);
    emitter.numberField("itemSpacing", style.itemSpacing);
    if (style.spacingHorizontal) emitter.numberField("candidateSpacingHorizontal", *style.spacingHorizontal);
    if (style.spacingVertical) emitter.numberField("candidateSpacingVertical", *style.spacingVertical);
    if (style.paddingHorizontal) emitter.numberField("candidatePaddingHorizontal", *style.paddingHorizontal);
    if (style.paddingVertical) emitter.numberField("candidatePaddingVertical", *style.paddingVertical);
    emitter.numberField("preeditSpacing", style.preeditSpacing);
    emitter.numberField("cornerRadius", style.cornerRadius);
    emitter.numberField("itemRadius", style.itemRadius);
    emitter.numberField("opacity", style.opacity);
    emitter.numberField("maxWidth", style.maxWidth);
    emitter.boolField("showLabels", style.showLabels);
    emitter.boolField("showComments", style.showComments);
    emitter.boolField("showPreedit", style.showPreedit);
    emitter.pair("background", style.background);
    emitter.pair("text", style.text);
    emitter.pair("accent", style.accent);
    if (style.decoration) {
        emitter.beginMember("decoration");
        emitter.beginObject();
        emitter.pair("backgroundEnd", style.decoration->backgroundEnd);
        emitter.pair("highlightMiddle", style.decoration->highlightMiddle);
        emitter.pair("highlightEnd", style.decoration->highlightEnd);
        emitter.pair("selectedText", style.decoration->selectedText);
        emitter.pair("secondaryText", style.decoration->secondaryText);
        emitter.endObject();
    }
    emitter.endObject();
    if (pretty) emitter.out += "\n";
    return emitter.out;
}

const std::vector<CandidateStyle>& builtinCandidateStyles() {
    static const std::vector<CandidateStyle> styles = [] {
        std::vector<CandidateStyle> list;

        // 「系统简约」 mirrors macOS/CandidateStyles/macos-default.json.
        {
            CandidateStyle style = documentDefaults();
            style.id = "system";
            style.name = "系统简约";
            style.horizontal = true;
            style.fontSize = 17;
            style.commentSize = 12;
            style.rowHeight = 30;
            style.padding = 6;
            style.itemSpacing = 4;
            style.spacingHorizontal = 4;
            style.spacingVertical = 4;
            style.paddingHorizontal = 10;
            style.paddingVertical = 4;
            style.preeditSpacing = 4;
            style.cornerRadius = 10;
            style.itemRadius = 6;
            style.opacity = 1;
            style.maxWidth = 600;
            style.showPreedit = false;
            style.background = ColorPair{StyleColor{1, 1, 1, 1}, StyleColor{0.0902, 0.1059, 0.1686, 1}};
            style.text = ColorPair{StyleColor{0.0824, 0.102, 0.1608, 1}, StyleColor{0.9412, 0.949, 0.9843, 1}};
            style.accent = ColorPair{StyleColor{0.3569, 0.2941, 0.9098, 1}, StyleColor{0.4275, 0.3608, 0.9412, 1}};
            style.decoration = CandidateDecoration{
                ColorPair{StyleColor{0.949, 0.9529, 0.9804, 1}, StyleColor{0.0549, 0.0667, 0.1098, 1}},
                ColorPair{StyleColor{0.2392, 0.349, 0.7686, 1}, StyleColor{0.298, 0.4039, 0.8196, 1}},
                ColorPair{StyleColor{0.0431, 0.4353, 0.5412, 1}, StyleColor{0.0863, 0.4784, 0.6196, 1}},
                ColorPair{StyleColor{1, 1, 1, 1}, StyleColor{1, 1, 1, 1}},
                ColorPair{StyleColor{0.4118, 0.4471, 0.5412, 1}, StyleColor{0.6667, 0.702, 0.7843, 1}}};
            list.push_back(style);
        }

        // 「星轨」: dark-first orbit console with a violet-to-teal selection.
        {
            CandidateStyle style = documentDefaults();
            style.id = "orbit";
            style.name = "星轨";
            style.horizontal = true;
            style.background = makePair(0xFFFFFF, 0x171B2B);
            style.text = makePair(0x151A29, 0xF0F2FB);
            style.accent = makePair(0x5B4BE8, 0x6D5CF0);
            style.fontSize = 17;
            style.commentSize = 11;
            style.rowHeight = 32;
            style.padding = 6;
            style.itemSpacing = 4;
            style.paddingHorizontal = 10;
            style.paddingVertical = 4;
            style.preeditSpacing = 4;
            style.cornerRadius = 11;
            style.itemRadius = 7;
            style.showPreedit = false;
            style.decoration = CandidateDecoration{
                makePair(0xF2F3FA, 0x0E111C),
                makePair(0x3D59C4, 0x4C67D1),
                makePair(0x0B6F8A, 0x167A9E),
                makePair(0xFFFFFF, 0xFFFFFF),
                makePair(0x69728A, 0xAAB3C8)};
            list.push_back(style);
        }

        // 「经典竖排」
        {
            CandidateStyle style = documentDefaults();
            style.id = "classic";
            style.name = "经典竖排";
            list.push_back(style);
        }

        // 「紧凑横排」
        {
            CandidateStyle style = documentDefaults();
            style.id = "compact";
            style.name = "紧凑横排";
            style.horizontal = true;
            style.fontSize = 15;
            style.commentSize = 11;
            style.rowHeight = 24;
            style.padding = 8;
            style.itemSpacing = 6;
            list.push_back(style);
        }

        // 「大字清晰」
        {
            CandidateStyle style = documentDefaults();
            style.id = "large";
            style.name = "大字清晰";
            style.fontSize = 24;
            style.commentSize = 17;
            style.rowHeight = 40;
            style.padding = 16;
            style.itemSpacing = 8;
            style.cornerRadius = 14;
            list.push_back(style);
        }

        // 「节庆流光」
        {
            CandidateStyle style = documentDefaults();
            style.id = "fiesta";
            style.name = "节庆流光";
            style.horizontal = true;
            style.fontName = "PingFang SC";
            style.fontSize = 16;
            style.commentSize = 12;
            style.rowHeight = 32;
            style.padding = 0;
            style.itemSpacing = 0;
            style.preeditSpacing = 0;
            style.cornerRadius = 14;
            style.itemRadius = 0;
            style.showPreedit = false;
            style.background = makePair(0xFFF5EC, 0x2A191B, 0.985, 0.985);
            style.text = makePair(0x402B2A, 0xFFF0E9);
            style.accent = makePair(0xBD382F, 0xFF8579);
            style.decoration = CandidateDecoration{
                makePair(0xEEF3FF, 0x17203A, 0.965, 0.965),
                makePair(0x3153AD, 0x94B3FF),
                makePair(0x816700, 0xF0D45F),
                makePair(0xFFFFFF, 0x391314),
                makePair(0x755951, 0xD0B1AB)};
            list.push_back(style);
        }
        return list;
    }();
    return styles;
}

bool parseCandidateStyle(std::string_view json, CandidateStyle& style, std::string& error) {
    if (json.size() > kMaxStyleBytes) {
        error = "候选样式文件不能超过 256 KiB。";
        return false;
    }
    if (!validUtf8(json)) {
        error = "候选样式不是有效的 UTF-8 文本。";
        return false;
    }
    JsonValue root;
    JsonParser parser(json);
    if (!parser.parse(root, error)) return false;
    if (root.type != JsonValue::Type::Object) {
        error = "候选样式必须是 JSON 对象。";
        return false;
    }
    std::string format;
    double version = 0;
    if (!readString(root, "format", format) || format != "xmjd.candidate-style") {
        error = "不是星猫候选样式（format 必须为 xmjd.candidate-style）。";
        return false;
    }
    if (!readNumber(root, "version", version) || static_cast<int>(version) != 1) {
        error = "候选样式版本不受支持（需要 version 1）。";
        return false;
    }
    CandidateStyle parsed = documentDefaults();
    readString(root, "id", parsed.id);
    readString(root, "name", parsed.name);
    std::string layout = parsed.horizontal ? "horizontal" : "vertical";
    if (readString(root, "layout", layout)) {
        if (layout == "horizontal") parsed.horizontal = true;
        else if (layout == "vertical") parsed.horizontal = false;
        else {
            error = "候选样式 layout 必须是 horizontal 或 vertical。";
            return false;
        }
    }
    readString(root, "fontName", parsed.fontName);
    readNumber(root, "fontSize", parsed.fontSize);
    readNumber(root, "commentSize", parsed.commentSize);
    readNumber(root, "rowHeight", parsed.rowHeight);
    readNumber(root, "padding", parsed.padding);
    readNumber(root, "itemSpacing", parsed.itemSpacing);
    readOptionalNumber(root, "candidateSpacingHorizontal", parsed.spacingHorizontal);
    readOptionalNumber(root, "candidateSpacingVertical", parsed.spacingVertical);
    readOptionalNumber(root, "candidatePaddingHorizontal", parsed.paddingHorizontal);
    readOptionalNumber(root, "candidatePaddingVertical", parsed.paddingVertical);
    readNumber(root, "preeditSpacing", parsed.preeditSpacing);
    readNumber(root, "cornerRadius", parsed.cornerRadius);
    readNumber(root, "itemRadius", parsed.itemRadius);
    readNumber(root, "opacity", parsed.opacity);
    readNumber(root, "maxWidth", parsed.maxWidth);
    readBool(root, "showLabels", parsed.showLabels);
    readBool(root, "showComments", parsed.showComments);
    readBool(root, "showPreedit", parsed.showPreedit);
    ColorPair colors = parsed.background;
    if (readPair(root.find("background"), colors)) parsed.background = colors;
    colors = parsed.text;
    if (readPair(root.find("text"), colors)) parsed.text = colors;
    colors = parsed.accent;
    if (readPair(root.find("accent"), colors)) parsed.accent = colors;
    if (const JsonValue* decoration = root.find("decoration")) {
        if (decoration->type != JsonValue::Type::Object) {
            error = "候选样式 decoration 必须是对象。";
            return false;
        }
        CandidateDecoration value{};
        value.backgroundEnd = parsed.background;
        value.highlightMiddle = parsed.accent;
        value.highlightEnd = parsed.accent;
        value.selectedText = ColorPair{StyleColor{1, 1, 1, 1}, StyleColor{1, 1, 1, 1}};
        value.secondaryText = parsed.text;
        bool valid = true;
        valid = readPair(decoration->find("backgroundEnd"), value.backgroundEnd) && valid;
        valid = readPair(decoration->find("highlightMiddle"), value.highlightMiddle) && valid;
        valid = readPair(decoration->find("highlightEnd"), value.highlightEnd) && valid;
        valid = readPair(decoration->find("selectedText"), value.selectedText) && valid;
        valid = readPair(decoration->find("secondaryText"), value.secondaryText) && valid;
        if (!valid) {
            error = "候选样式 decoration 颜色不完整（需要 light/dark 的 red、green、blue）。";
            return false;
        }
        parsed.decoration = value;
    }
    if (!validCandidateStyle(parsed)) {
        error = "候选样式的名称、尺寸或颜色超出允许范围。";
        return false;
    }
    style = std::move(parsed);
    return true;
}

bool validCandidateStyle(const CandidateStyle& style) {
    if (style.format != "xmjd.candidate-style" || style.version != 1) return false;
    if (style.id.empty() || style.id.size() > 100) return false;
    if (style.name.empty() || style.name.size() > 240) return false;
    if (style.fontName.size() > 150) return false;
    const auto inRange = [](double value, double low, double high) {
        return std::isfinite(value) && value >= low && value <= high;
    };
    if (!inRange(style.fontSize, 10, 48)) return false;
    if (!inRange(style.commentSize, 10, 48)) return false;
    if (!inRange(style.rowHeight, 20, 96)) return false;
    if (!inRange(style.padding, 0, 48)) return false;
    if (!inRange(style.itemSpacing, 0, 48)) return false;
    for (const auto* optional : {&style.spacingHorizontal, &style.spacingVertical,
                                 &style.paddingHorizontal, &style.paddingVertical}) {
        if (optional->has_value() && !inRange(**optional, 0, 48)) return false;
    }
    if (!inRange(style.preeditSpacing, 0, 48)) return false;
    if (!inRange(style.cornerRadius, 0, 48)) return false;
    if (!inRange(style.itemRadius, 0, 48)) return false;
    if (!inRange(style.opacity, 0.3, 1)) return false;
    if (!inRange(style.maxWidth, 180, 1000)) return false;
    for (const auto* pair : {&style.background, &style.text, &style.accent}) {
        if (!colorInRange(pair->light) || !colorInRange(pair->dark)) return false;
    }
    if (style.decoration) {
        for (const auto* pair : {&style.decoration->backgroundEnd, &style.decoration->highlightMiddle,
                                 &style.decoration->highlightEnd, &style.decoration->selectedText,
                                 &style.decoration->secondaryText}) {
            if (!colorInRange(pair->light) || !colorInRange(pair->dark)) return false;
        }
    }
    return true;
}

std::filesystem::path candidateStyleDirectory(const std::filesystem::path& dataRoot) {
    return dataRoot / L"CandidateStyles";
}

std::filesystem::path activeCandidateStylePath(const std::filesystem::path& dataRoot) {
    return candidateStyleDirectory(dataRoot) / L"active.json";
}

CandidateStyle loadActiveCandidateStyle(const std::filesystem::path& dataRoot) {
    const auto& builtin = builtinCandidateStyles();
    std::string contents;
    if (!readStyleFile(activeCandidateStylePath(dataRoot), contents)) return builtin.front();
    CandidateStyle style;
    std::string error;
    if (!parseCandidateStyle(contents, style, error)) return builtin.front();
    return style;
}

CandidateStyleCatalog loadCandidateStyleCatalog(const std::filesystem::path& dataRoot) {
    CandidateStyleCatalog catalog;
    catalog.builtin = builtinCandidateStyles();
    catalog.active = catalog.builtin.front();
    std::error_code ec;
    const auto directory = candidateStyleDirectory(dataRoot);
    std::filesystem::create_directories(directory, ec);
    {
        std::string contents;
        if (readStyleFile(activeCandidateStylePath(dataRoot), contents)) {
            CandidateStyle parsed;
            std::string error;
            if (parseCandidateStyle(contents, parsed, error)) catalog.active = parsed;
            else catalog.notice = L"上次使用的皮肤无法读取，已回退到「系统简约」；原文件仍保留。";
        }
    }
    std::vector<std::filesystem::path> files;
    for (std::filesystem::directory_iterator it(directory, ec), end; it != end && !ec; it.increment(ec)) {
        if (!it->is_regular_file(ec) || ec) continue;
        if (it->path().extension() != L".json") continue;
        files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    if (files.size() > kMaxCustomStyles) {
        catalog.notice = L"只加载前 128 个自定义皮肤，其余文件仍保留在皮肤目录中。";
    }
    std::size_t loaded = 0;
    for (const auto& file : files) {
        if (loaded >= kMaxCustomStyles) break;
        if (file.filename() == L"active.json") continue;
        std::string contents;
        if (!readStyleFile(file, contents)) continue;
        CandidateStyle parsed;
        std::string error;
        if (!parseCandidateStyle(contents, parsed, error)) {
            catalog.notice = L"有皮肤文件未能加载，已保留原文件：" + file.filename().wstring();
            continue;
        }
        if (std::any_of(catalog.builtin.begin(), catalog.builtin.end(),
                        [&](const CandidateStyle& style) { return style.id == parsed.id; })) {
            continue;
        }
        ++loaded;
        catalog.custom.push_back(std::move(parsed));
    }
    return catalog;
}

bool writeCandidateStyleFile(const CandidateStyle& style, const std::filesystem::path& path, std::string& error) {
    if (!validCandidateStyle(style)) {
        error = "皮肤内容超出允许范围，未写入。";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "无法创建皮肤目录：" + toUtf8(path.parent_path().wstring());
        return false;
    }
    const auto temporary = path.wstring() + L".tmp";
    {
        std::ofstream stream(std::filesystem::path(temporary), std::ios::binary | std::ios::trunc);
        if (!stream) {
            error = "无法写入皮肤文件：" + toUtf8(path.wstring());
            return false;
        }
        const std::string contents = serializeCandidateStyle(style, true);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!stream.good()) {
            error = "无法写入皮肤文件：" + toUtf8(path.wstring());
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "无法替换皮肤文件：" + toUtf8(path.wstring());
        return false;
    }
    return true;
}

bool activateCandidateStyle(const std::filesystem::path& dataRoot, const CandidateStyle& style, std::string& error) {
    return writeCandidateStyleFile(style, activeCandidateStylePath(dataRoot), error);
}

int stylePixels(double value, int dpi) {
    const double scale = static_cast<double>(dpi <= 0 ? 96 : dpi) / 96.0;
    return static_cast<int>(std::lround(value * scale));
}

CandidateMetrics candidateMetrics(const CandidateStyle& style, int dpi) {
    CandidateMetrics metrics;
    metrics.fontSize = std::max(1, stylePixels(style.fontSize, dpi));
    metrics.labelSize = std::max(1, stylePixels(style.commentSize, dpi));
    metrics.rowHeight = std::max(metrics.fontSize, stylePixels(style.rowHeight, dpi));
    metrics.padding = std::max(0, stylePixels(style.padding, dpi));
    metrics.padX = std::max(0, stylePixels(style.horizontalPadding(), dpi));
    metrics.spacingH = std::max(0, stylePixels(style.horizontalSpacing(), dpi));
    metrics.spacingV = std::max(0, stylePixels(style.verticalSpacing(), dpi));
    metrics.maxWidth = std::max(120, stylePixels(style.maxWidth, dpi));
    metrics.labelGap = style.showLabels ? std::max(2, metrics.fontSize / 5) : 0;
    metrics.commentGap = style.showComments ? std::max(3, metrics.fontSize / 4) : 0;
    return metrics;
}

CandidateLayout layoutCandidates(const CandidateStyle& style, const std::vector<CandidateItem>& items,
                                 int dpi, const CandidateTextMeasure& measure) {
    CandidateLayout layout;
    if (items.empty()) return layout;
    const CandidateMetrics metrics = candidateMetrics(style, dpi);
    const int padding = metrics.padding;
    const int maxWidth = metrics.maxWidth;

    std::vector<int> widths;
    widths.reserve(items.size());
    for (const auto& item : items) {
        int width = 0;
        if (style.showLabels && !item.label.empty()) {
            width += measure(item.label, metrics.labelSize) + metrics.labelGap;
        }
        width += measure(item.text, metrics.fontSize);
        if (style.showComments && !item.comment.empty()) {
            width += metrics.commentGap + measure(item.comment, metrics.labelSize);
        }
        widths.push_back(std::max(0, width));
    }

    layout.contentLeft = padding;
    layout.contentTop = padding;
    if (!style.horizontal) {
        const int widest = *std::max_element(widths.begin(), widths.end());
        layout.width = std::min(maxWidth, widest + metrics.padX * 2 + padding * 2);
        int y = padding;
        for (std::size_t index = 0; index < items.size(); ++index) {
            CandidateBox box;
            box.x = padding;
            box.y = y;
            box.width = std::max(1, layout.width - padding * 2);
            box.height = metrics.rowHeight;
            layout.items.push_back(box);
            y += metrics.rowHeight + metrics.spacingV;
        }
        layout.height = y - metrics.spacingV + padding;
        return layout;
    }

    const int available = std::max(1, maxWidth - padding * 2);
    int x = padding;
    int y = padding;
    int widestRow = 0;
    for (std::size_t index = 0; index < items.size(); ++index) {
        const int itemWidth = widths[index] + metrics.padX * 2;
        if (x != padding && x + itemWidth > padding + available) {
            widestRow = std::max(widestRow, x - metrics.spacingH);
            x = padding;
            y += metrics.rowHeight + metrics.spacingV;
        }
        CandidateBox box;
        box.x = x;
        box.y = y;
        box.width = itemWidth;
        box.height = metrics.rowHeight;
        layout.items.push_back(box);
        widestRow = std::max(widestRow, x + itemWidth);
        x += itemWidth + metrics.spacingH;
    }
    layout.width = std::min(maxWidth, widestRow + padding);
    for (auto& box : layout.items) {
        box.width = std::max(1, std::min(box.width, layout.width - padding - box.x));
    }
    layout.height = y + metrics.rowHeight + padding;
    return layout;
}

} // namespace xmjd
