//
// Created by fang on 2022/11/19.
//

#include <pystring.h>
#include <cstdlib>
#include <regex>
#include <borealis/core/application.hpp>
#include <borealis/views/rectangle.hpp>
#include <borealis/views/image.hpp>

#include "fragment/latest_update.hpp"
#include "analytics.h"
#include "utils/config_helper.hpp"
#include "utils/image_helper.hpp"
#include "view/text_box.hpp"

static inline bool isSeparator(const std::string& text) {
    return std::all_of(text.begin(), text.end(), [](char c) { return c == ' ' || c == '-'; });
}

static inline bool isListItem(const std::string& text) {
    return pystring::startswith(pystring::lstrip(text, " "), "-");
}

static inline int getStartSpace(const std::string& text) { return pystring::find(text, "-"); }

static inline bool isHeader(const std::string& text) { return pystring::startswith(text, "#"); }

static inline size_t getHeaderSize(const std::string& text) {
    size_t res = text.find_first_not_of('#');
    if (res < 1) return 1;
    if (res > 5) return 5;
    return res;
}

static inline bool isImage(const std::string& text) { return pystring::startswith(pystring::lstrip(text), "!["); }

static int parseImageNumber(const std::string& value, int fallback) {
    if (value.empty())
        return fallback;

    char* end  = nullptr;
    long parsed = std::strtol(value.c_str(), &end, 10);
    return end != value.c_str() && *end == '\0' ? static_cast<int>(parsed) : fallback;
}

#define PARSE_IMAGE_DATA(index, name, def)                           \
    if (data.size() > (index)) {                                     \
        (name) = parseImageNumber(data[index], def);                  \
    } else {                                                         \
        (name) = def;                                                \
    }
static inline std::string getImageUrl(const std::string& test, int& maxHeight, int& margin) {
    std::regex re(R"(\!\[(.*)\]\((.*)\))");
    std::smatch match;
    if (std::regex_search(test, match, re)) {
        auto data = pystring::split(match[1], ",");
        PARSE_IMAGE_DATA(0, maxHeight, 200)
        PARSE_IMAGE_DATA(1, margin, 10)
        return match[2];
    }
    return "";
}

#define SHOW_REACTION(emoji, count)          \
    if ((count) > 0) {                       \
        auto reaction = new brls::Label();   \
        auto num      = new brls::Label();   \
        reaction->setText(emoji);            \
        num->setText(std::to_string(count)); \
        num->setMarginLeft(10);              \
        num->setMarginRight(20);             \
        topBox->addView(reaction);           \
        topBox->addView(num);                \
    }

LatestUpdate::LatestUpdate(const ReleaseNote& info) {
    this->inflateFromXMLRes("xml/fragment/latest_update.xml");
    brls::Logger::debug("Fragment LatestUpdate: create");

    header->setText(info.name);
    subtitle->setText(fmt::format("新版本已发布，您现在的版本是: {}", APPVersion::instance().git_tag));
    author->setUserInfo(info.author.avatar_url, info.author.login, info.published_at);

    // 显示来自 github 的 reactions
    SHOW_REACTION("👍", info.reactions.plus_one);
    SHOW_REACTION("😀", info.reactions.laugh);
    SHOW_REACTION("🎉", info.reactions.hooray);
    SHOW_REACTION("❤️", info.reactions.heart);
    SHOW_REACTION("🚀", info.reactions.rocket);
    SHOW_REACTION("👀", info.reactions.eyes);

    // 简单解析 markdown
    auto theme     = brls::Application::getTheme();
    auto lineColor = theme.getColor("brls/applet_frame/separator");
    auto content   = pystring::replace(info.body, "\r\n", "\n");
    auto lines     = pystring::split(content, "\n");
    for (auto& l : lines) {
        if (l.empty()) {
            // 空行
            auto space = new brls::Rectangle(nvgRGBA(0, 0, 0, 0));
            space->setHeight(20);
            textbox->addView(space);
        } else if (isSeparator(l)) {
            // 分割线
            auto space = new brls::Rectangle(lineColor);
            space->setHeight(1);
            textbox->addView(space);
        } else if (isListItem(l)) {
            // 列表项
            auto linePoint = new brls::Label();
            linePoint->setText("•");
            linePoint->setMarginRight(10);
            linePoint->setVerticalAlign(brls::VerticalAlign::TOP);

            auto line = new TextBox();
            int start = getStartSpace(l);
            line->setText(l.substr(start + 1));
            line->setMarginBottom(10);
            line->setGrow(1);

            auto lineBox = new brls::Box();
            lineBox->setMarginLeft((float)start * 10);
            lineBox->addView(linePoint);
            lineBox->addView(line);
            textbox->addView(lineBox);
        } else if (isImage(l)) {
            // 图片
            int maxHeight = 0, margin = 0;
            auto url = getImageUrl(l, maxHeight, margin);
            if (url.empty()) continue;
            auto box   = new brls::Box();
            auto image = new brls::Image();
            image->setMaxHeight((float)maxHeight);
            image->setMargins((float)margin, (float)margin, (float)margin, (float)margin);
            ImageHelper::with(image)->load(url);
            box->addView(image);
            box->setJustifyContent(brls::JustifyContent::CENTER);
            textbox->addView(box);
        } else {
            auto line = new brls::Label();
            line->setMarginBottom(10);
            if (isHeader(l)) {
                // 标题
                size_t size = getHeaderSize(l);
                line->setFontSize(48 - 6 * (float)size);
                line->setMarginBottom(20 - 2 * (float)size);
                line->setText(pystring::lstrip(l, "# "));
            } else {
                // 普通文字
                line->setText(l);
            }
            textbox->addView(line);
        }
    }

    GA("show_update_dialog")
}

LatestUpdate::~LatestUpdate() { brls::Logger::debug("Fragment LatestUpdate: delete"); }
