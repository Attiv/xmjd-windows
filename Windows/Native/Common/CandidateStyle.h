#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace xmjd {

// Candidate skins use the same document format as the Mac build
// ("xmjd.candidate-style" v1) so a skin exported on one platform can be
// imported on the other without translation.
struct StyleColor {
    double red = 0;
    double green = 0;
    double blue = 0;
    double alpha = 1;
};

struct ColorPair {
    StyleColor light;
    StyleColor dark;
};

struct CandidateDecoration {
    ColorPair backgroundEnd;
    ColorPair highlightMiddle;
    ColorPair highlightEnd;
    ColorPair selectedText;
    ColorPair secondaryText;
};

struct CandidateStyle {
    std::string format = "xmjd.candidate-style";
    int version = 1;
    std::string id = "system";
    std::string name = "系统简约";
    bool horizontal = false;
    std::string fontName;
    double fontSize = 17;
    double commentSize = 12;
    double rowHeight = 30;
    double padding = 6;
    double itemSpacing = 4;
    std::optional<double> spacingHorizontal;
    std::optional<double> spacingVertical;
    std::optional<double> paddingHorizontal;
    std::optional<double> paddingVertical;
    double preeditSpacing = 4;
    double cornerRadius = 10;
    double itemRadius = 6;
    double opacity = 1;
    double maxWidth = 600;
    bool showLabels = true;
    bool showComments = true;
    bool showPreedit = false;
    ColorPair background;
    ColorPair text;
    ColorPair accent;
    std::optional<CandidateDecoration> decoration;

    double horizontalSpacing() const { return spacingHorizontal.value_or(itemSpacing); }
    double verticalSpacing() const { return spacingVertical.value_or(itemSpacing); }
    double horizontalPadding() const { return paddingHorizontal.value_or(decoration ? 10.0 : 7.0); }
    double verticalPadding() const { return paddingVertical.value_or(4); }

    bool displaysPreedit(bool inlineComposition) const { return showPreedit || !inlineComposition; }
};

struct CandidateStyleCatalog {
    CandidateStyle active;
    std::vector<CandidateStyle> builtin;
    std::vector<CandidateStyle> custom;
    std::wstring notice;
};

const std::vector<CandidateStyle>& builtinCandidateStyles();

// Parse a Mac/Windows skin document. Returns false and fills `error` when the
// document is not valid UTF-8 JSON, has the wrong format/version, or carries
// out-of-range geometry.
bool parseCandidateStyle(std::string_view json, CandidateStyle& style, std::string& error);
std::string serializeCandidateStyle(const CandidateStyle& style, bool pretty);
bool validCandidateStyle(const CandidateStyle& style);

std::filesystem::path candidateStyleDirectory(const std::filesystem::path& dataRoot);
std::filesystem::path activeCandidateStylePath(const std::filesystem::path& dataRoot);
// Reads CandidateStyles\active.json, falling back to the built-in catalog.
CandidateStyle loadActiveCandidateStyle(const std::filesystem::path& dataRoot);
// Loads active.json plus every *.json in the style directory (bounded).
CandidateStyleCatalog loadCandidateStyleCatalog(const std::filesystem::path& dataRoot);
bool writeCandidateStyleFile(const CandidateStyle& style, const std::filesystem::path& path, std::string& error);
bool activateCandidateStyle(const std::filesystem::path& dataRoot, const CandidateStyle& style, std::string& error);

struct CandidateItem {
    std::wstring label;
    std::wstring text;
    std::wstring comment;
};

struct CandidateBox {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct CandidateLayout {
    int width = 0;
    int height = 0;
    int preeditHeight = 0;
    int contentLeft = 0;
    int contentTop = 0;
    std::vector<CandidateBox> items;
};

// Measures one run of text at `pixelSize`; the renderer supplies the real font
// metrics. Tests inject a deterministic width so the flow math is checkable
// without a window station.
using CandidateTextMeasure = std::function<int(std::wstring_view text, int pixelSize)>;

// Device-pixel values derived from a style; shared by layout and painting so
// both agree on paddings and gaps.
struct CandidateMetrics {
    int fontSize = 17;
    int labelSize = 13;
    int rowHeight = 28;
    int padding = 12;
    int padX = 7;
    int spacingH = 4;
    int spacingV = 4;
    int maxWidth = 600;
    int labelGap = 3;
    int commentGap = 4;
};

CandidateMetrics candidateMetrics(const CandidateStyle& style, int dpi);

// Pure layout math shared by painting and hit-testing. Sizes in the style are
// 96-DPI points and are scaled by `dpi`.
CandidateLayout layoutCandidates(const CandidateStyle& style, const std::vector<CandidateItem>& items,
                                 int dpi, const CandidateTextMeasure& measure);

// Scales a style dimension from 96-DPI points to device pixels.
int stylePixels(double value, int dpi);

} // namespace xmjd
