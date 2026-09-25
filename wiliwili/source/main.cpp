/**

██     ██ ██ ██      ██ ██     ██ ██ ██      ██
██     ██ ██ ██      ██ ██     ██ ██ ██      ██
██  █  ██ ██ ██      ██ ██  █  ██ ██ ██      ██
██ ███ ██ ██ ██      ██ ██ ███ ██ ██ ██      ██
 ███ ███  ██ ███████ ██  ███ ███  ██ ███████ ██

 Licensed under the GPL-3.0 license
*/

#include <borealis.hpp>

#include "utils/config_helper.hpp"
#include "utils/activity_helper.hpp"
#include "view/mpv_core.hpp"

#ifdef IOS
#include <SDL2/SDL_main.h>
#endif

#ifdef __XBOX360__
#include <cstdlib>
#include <exception>
#include <typeinfo>
#endif

int main(int argc, char* argv[]) {
#ifdef __XBOX360__
    // OXDK's default std::terminate handler is a null pointer: any uncaught
    // exception or failed dynamic_cast would jump to address 0 with no
    // diagnostic. Install an abort handler so the terminate reason is at
    // least visible in the XBDM debug output before the title dies.
    std::set_terminate([]() {
        brls::Logger::error("std::terminate called - aborting");
        std::abort();
    });
#endif

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "-d") == 0) {
            brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        } else if (std::strcmp(argv[i], "-v") == 0) {
            brls::Application::enableDebuggingView(true);
        } else if (std::strcmp(argv[i], "-t") == 0) {
            MPVCore::TERMINAL = true;
        } else if (std::strcmp(argv[i], "-o") == 0) {
            const char* path = (i + 1 < argc) ? argv[++i] : "wiliwili.log";
            brls::Logger::setLogOutput(std::fopen(path, "w+"));
        }
    }

    // Load cookies and settings
    ProgramConfig::instance().init();

    // Init the app and i18n
    if (!brls::Application::init()) {
        brls::Logger::error("Unable to init application");
        return EXIT_FAILURE;
    }

    // Return directly to the desktop when closing the application (only for NX)
    brls::Application::getPlatform()->exitToHomeMode(true);

    brls::Application::createWindow("wiliwili");
    brls::Logger::info("createWindow done");
#ifdef __XBOX360__
    // Dump the Itanium RTTI vtable slots before XML inflation. This identifies
    // a bad XDK typeinfo table directly on hardware instead of relying on the
    // later illegal-indirect-call address alone.
    const std::type_info& boxType = typeid(brls::Box);
    const std::type_info& viewType = typeid(brls::View);
    auto dumpRtti = [](const char* name, const std::type_info& type) {
        auto vtable = *reinterpret_cast<const uintptr_t* const*>(&type);
        brls::Logger::info("Xbox RTTI {} type={:p} vtable={:p} slots={:p},{:p},{:p},{:p},{:p},{:p}",
                           name, static_cast<const void*>(&type), static_cast<const void*>(vtable),
                           reinterpret_cast<const void*>(vtable[0]), reinterpret_cast<const void*>(vtable[1]),
                           reinterpret_cast<const void*>(vtable[2]), reinterpret_cast<const void*>(vtable[3]),
                           reinterpret_cast<const void*>(vtable[4]), reinterpret_cast<const void*>(vtable[5]));
    };
    dumpRtti("Box", boxType);
    dumpRtti("View", viewType);
#endif

    // Register custom view\theme\style
    Register::initCustomView();
    Register::initCustomTheme();
    Register::initCustomStyle();

    brls::Application::getPlatform()->disableScreenDimming(false);

    if (brls::Application::getPlatform()->isApplicationMode()) {
#if defined(PLATFORM_XBOX360) || defined(__XBOX360__)
        // Keep the first Xbox 360 vertical slice offline and deterministic.
        // HintActivity exercises XML inflation, images, focus navigation and
        // the GalleryView without requiring the unfinished API/player layers.
        Intent::openHint();
#else
        Intent::openMain();
#endif
        // Uncomment these lines to debug activities
        //        Intent::openBV("BV1Da411Y7U4");  // 弹幕防遮挡 (横屏)
        //        Intent::openBV("BV1iN4y1m7J3");  // 弹幕防遮挡 (竖屏)
        //        Intent::openBV("BV1kT4y1s7od");  // 高级弹幕 测试0
        //        Intent::openBV("BV1eN4y147bC");  // 高级弹幕 测试1
        //        Intent::openBV("BV16x411D7NK");  // 高级弹幕 测试2
        //        Intent::openBV("BV1uW411e7gt");  // bas 弹幕 (Bilibili Animation Script)
        //        Intent::openBV("BV1zb4y1j7vz");  // flv 模式报错：HTTP 424
        //        Intent::openBV("BV1jL41167ZG");  // 充电视频
        //        Intent::openBV("BV1dx411c7Av");  // flv拼接视频
        //        Intent::openBV("BV15z4y1Z734");  // 4K HDR 视频
        //        Intent::openBV("BV1qM4y1w716");  // 8K
        //        Intent::openBV("BV1PN4y1G7u2");  // up主视频自动跳转番剧
        //        Intent::openBV("BV1sK411s7zq");  // 多P视频测试
        //        Intent::openBV("BV1Cg411j76F");  // 多字幕测试
        //        Intent::openBV("BV1A44y1u7PF");  // 测试FFMPEG在switch上的bug（加载时间过长）
        //        Intent::openBV("BV1eD4y1b7Jv");  // 测试 MPV 在switch上的bug（长时间播放崩溃）
        //        Intent::openBV("BV1U3411c7Qx");  // 测试长标题
        //        Intent::openBV("BV1fG411W7Px");  // 测试弹幕
        //        Intent::openSeasonByEpId(323434);// 测试电影
        //        Intent::openLive(1942240);       // 测试直播
        //        Intent::openSearch("harry");     // 测试搜索影片
        //        Intent::openTVSearch();          // 测试TV搜索模式
        //        Intent::openHint();              // 应用开启教程页面
        //        Intent::openCollection("2511565362"); // 测试打开收藏夹
        //        Intent::openPgcFilter("/page/home/pgc/more?type=2&index_type=2&area=2&order=2&season_status=-1&season_status=3,6"); // 影片分类索引
        //        Intent::openSetting();  //  设置页面
    } else {
        Intent::openHint();
    }

#if !defined(PLATFORM_XBOX360) && !defined(__XBOX360__)
    GA("open_app", {{"version", APPVersion::instance().getVersionStr()},
                    {"language", brls::Application::getLocale()},
                    {"window", fmt::format("{}x{}", brls::Application::windowWidth, brls::Application::windowHeight)}})
    APPVersion::instance().checkUpdate();
#endif

    // Run the app
    // brls::Application::setLimitedFPS(60);
    while (brls::Application::mainLoop()) {
    }

    brls::Logger::info("mainLoop done");

    // Cleanup curl and Check whether restart is required
    ProgramConfig::instance().exit(argv);

    // Exit
    return EXIT_SUCCESS;
}

#ifdef __WINRT__
#include <borealis/core/main.hpp>
#endif
