#include "../Native/Common/CandidateStyle.h"

#include <cmath>
#include <iostream>
#include <string>
#include <vector>

#define CHECK(condition)                                                                                                      \
    do {                                                                                                                     \
        if (!(condition)) {                                                                                                  \
            std::cerr << "CHECK failed: " #condition << " at " << __FILE__ << ':' << __LINE__ << '\n';                    \
            return 1;                                                                                                      \
        }                                                                                                                  \
    } while (false)

namespace {
using namespace xmjd;

// A document saved by the Mac style editor, including decoration.
const char* kMacDocument = R"json({
  "format": "xmjd.candidate-style",
  "version": 1,
  "id": "macos-default",
  "name": "系统简约",
  "layout": "horizontal",
  "fontName": "",
  "fontSize": 17,
  "commentSize": 12,
  "rowHeight": 30,
  "padding": 6,
  "itemSpacing": 4,
  "candidateSpacingHorizontal": 4,
  "candidateSpacingVertical": 4,
  "candidatePaddingHorizontal": 10,
  "candidatePaddingVertical": 4,
  "preeditSpacing": 4,
  "cornerRadius": 10,
  "itemRadius": 6,
  "opacity": 1,
  "maxWidth": 600,
  "showLabels": true,
  "showComments": true,
  "showPreedit": false,
  "background": {
    "light": {"red": 1, "green": 1, "blue": 1, "alpha": 1},
    "dark": {"red": 0.0901960784, "green": 0.1058823529, "blue": 0.168627451, "alpha": 1}
  },
  "text": {
    "light": {"red": 0.0823529412, "green": 0.1019607843, "blue": 0.1607843137, "alpha": 1},
    "dark": {"red": 0.9411764706, "green": 0.9490196078, "blue": 0.9843137255, "alpha": 1}
  },
  "accent": {
    "light": {"red": 0.3568627451, "green": 0.2941176471, "blue": 0.9098039216, "alpha": 1},
    "dark": {"red": 0.4274509804, "green": 0.3607843137, "blue": 0.9411764706, "alpha": 1}
  },
  "decoration": {
    "backgroundEnd": {
      "light": {"red": 0.9490196078, "green": 0.9529411765, "blue": 0.9803921569, "alpha": 1},
      "dark": {"red": 0.0549019608, "green": 0.0666666667, "blue": 0.1098039216, "alpha": 1}
    },
    "highlightMiddle": {
      "light": {"red": 0.2392156863, "green": 0.3490196078, "blue": 0.768627451, "alpha": 1},
      "dark": {"red": 0.2980392157, "green": 0.4039215686, "blue": 0.8196078431, "alpha": 1}
    },
    "highlightEnd": {
      "light": {"red": 0.0431372549, "green": 0.4352941176, "blue": 0.5411764706, "alpha": 1},
      "dark": {"red": 0.0862745098, "green": 0.4784313725, "blue": 0.6196078431, "alpha": 1}
    },
    "selectedText": {
      "light": {"red": 1, "green": 1, "blue": 1, "alpha": 1},
      "dark": {"red": 1, "green": 1, "blue": 1, "alpha": 1}
    },
    "secondaryText": {
      "light": {"red": 0.4117647059, "green": 0.4470588235, "blue": 0.5411764706, "alpha": 1},
      "dark": {"red": 0.6666666667, "green": 0.7019607843, "blue": 0.7843137255, "alpha": 1}
    }
  }
})json";

int fakeMeasure(std::wstring_view text, int pixelSize) {
    // Deterministic: CJK-ish glyphs are one em wide, everything else half an em.
    int width = 0;
    for (const wchar_t character : text) {
        width += character >= 0x2E80 ? pixelSize : pixelSize / 2;
    }
    return width;
}

int run() {
    CandidateStyle style;
    std::string error;
    CHECK(parseCandidateStyle(kMacDocument, style, error));
    CHECK(style.id == "macos-default");
    CHECK(style.horizontal);
    CHECK(std::abs(style.fontSize - 17) < 0.001);
    CHECK(style.decoration.has_value());
    CHECK(std::abs(style.decoration->highlightEnd.dark.green - 0.4784313725) < 1e-9);
    CHECK(std::abs(style.background.dark.blue - 0.168627451) < 1e-9);
    CHECK(style.horizontalSpacing() == 4);
    CHECK(style.horizontalPadding() == 10);
    CHECK(!style.displaysPreedit(true));

    // Re-serialising and re-reading must preserve the document.
    const std::string text = serializeCandidateStyle(style, true);
    CandidateStyle again;
    CHECK(parseCandidateStyle(text, again, error));
    CHECK(again.id == style.id && again.name == style.name);
    CHECK(again.horizontal == style.horizontal);
    CHECK(again.decoration.has_value());
    CHECK(std::abs(again.rowHeight - style.rowHeight) < 1e-9);
    CHECK(std::abs(again.decoration->secondaryText.light.red - style.decoration->secondaryText.light.red) < 1e-9);
    const std::string compact = serializeCandidateStyle(style, false);
    CHECK(compact.find('\n') == std::string::npos);
    CandidateStyle fromCompact;
    CHECK(parseCandidateStyle(compact, fromCompact, error));

    // Wrong format, missing version and out-of-range geometry are rejected.
    CandidateStyle rejected;
    CHECK(!parseCandidateStyle(R"({"format":"xmjd.keyboard-theme","version":1,"id":"x","name":"y"})", rejected, error));
    CHECK(!parseCandidateStyle(R"({"format":"xmjd.candidate-style","version":2,"id":"x","name":"y"})", rejected, error));
    CHECK(!parseCandidateStyle(
        R"({"format":"xmjd.candidate-style","version":1,"id":"","name":"y","fontSize":17})", rejected, error));
    CHECK(!parseCandidateStyle(
        R"({"format":"xmjd.candidate-style","version":1,"id":"x","name":"y","fontSize":900})", rejected, error));
    CHECK(!parseCandidateStyle("not json", rejected, error));

    // Unicode escapes survive the parser (skin names may be escaped by editors).
    CandidateStyle escaped;
    CHECK(parseCandidateStyle(
        R"({"format":"xmjd.candidate-style","version":1,"id":"x","name":"\u661f\u8f68","layout":"vertical"})",
        escaped, error));
    CHECK(escaped.name == "星轨");
    CHECK(!escaped.horizontal);

    // Missing optional decoration means a flat skin, not the built-in gradient.
    CandidateStyle flat;
    CHECK(parseCandidateStyle(
        R"({"format":"xmjd.candidate-style","version":1,"id":"flat","name":"扁平","layout":"vertical"})",
        flat, error));
    CHECK(!flat.decoration.has_value());
    CHECK(flat.horizontalPadding() == 7);

    // Every built-in preset must survive a round trip.
    for (const auto& preset : builtinCandidateStyles()) {
        CHECK(validCandidateStyle(preset));
        CandidateStyle copy;
        CHECK(parseCandidateStyle(serializeCandidateStyle(preset, true), copy, error));
        CHECK(copy.id == preset.id);
        CHECK(copy.decoration.has_value() == preset.decoration.has_value());
    }

    // Vertical layout: one box per candidate, width capped by maxWidth.
    {
        std::vector<CandidateItem> items;
        items.push_back(CandidateItem{L"1", L"你", L""});
        items.push_back(CandidateItem{L"2", L"你好", L"ni hao"});
        const CandidateStyle vertical = builtinCandidateStyles()[2];
        const CandidateLayout layout = layoutCandidates(vertical, items, 96, fakeMeasure);
        CHECK(layout.items.size() == 2);
        CHECK(layout.width <= stylePixels(vertical.maxWidth, 96));
        CHECK(layout.items[1].y > layout.items[0].y);
        CHECK(layout.items[0].x == layout.items[1].x);
        CHECK(layout.height > 0);
        CHECK(layout.items[1].y + layout.items[1].height <= layout.height);
    }

    // Horizontal layout wraps before exceeding maxWidth and never overlaps.
    {
        CandidateStyle horizontal = builtinCandidateStyles()[0];
        horizontal.maxWidth = 200;
        std::vector<CandidateItem> items;
        for (int index = 0; index < 10; ++index) {
            items.push_back(CandidateItem{std::to_wstring(index + 1), L"候选词语", L"comment"});
        }
        const CandidateLayout layout = layoutCandidates(horizontal, items, 96, fakeMeasure);
        CHECK(layout.items.size() == items.size());
        CHECK(layout.width <= stylePixels(horizontal.maxWidth, 96));
        int rows = 1;
        for (std::size_t index = 1; index < layout.items.size(); ++index) {
            const auto& previous = layout.items[index - 1];
            const auto& current = layout.items[index];
            if (current.y != previous.y) {
                ++rows;
                CHECK(current.x == layout.contentLeft);
                CHECK(current.y > previous.y);
            } else {
                CHECK(current.x >= previous.x + previous.width);
            }
            CHECK(current.x + current.width <= layout.width);
        }
        CHECK(rows > 1);
        CHECK(layout.height >= rows * stylePixels(horizontal.rowHeight, 96));
    }

    // DPI scaling changes pixels, not the item count.
    {
        std::vector<CandidateItem> items{CandidateItem{L"1", L"你", L""}};
        const CandidateStyle style96 = builtinCandidateStyles()[0];
        const CandidateLayout at96 = layoutCandidates(style96, items, 96, fakeMeasure);
        const CandidateLayout at192 = layoutCandidates(style96, items, 192, fakeMeasure);
        CHECK(at192.width > at96.width);
        CHECK(at192.height == at96.height * 2);
    }
    return 0;
}
} // namespace

int main() {
    try {
        return run();
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
}
