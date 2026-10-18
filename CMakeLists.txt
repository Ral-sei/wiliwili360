cmake_minimum_required(VERSION 3.13)

set(BOREALIS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis)
set(BOREALIS_LIBRARY ${BOREALIS_DIR}/library)

# Include Platform common options
# wiliwili supports desktop, Switch, and the Xbox 360/XDK bring-up path
# example: cmake -B build -DPLATFORM_DESKTOP=ON
include(${BOREALIS_LIBRARY}/cmake/commonOption.cmake)

# NintendoSwitch
cmake_dependent_option(BUILTIN_NSP "Built in NSP forwarder" OFF "PLATFORM_SWITCH" OFF)

# macOS
# These four options are only for macOS app bundling usage.
# Do not use it until you know what you are doing.
cmake_dependent_option(MAC_IntelChip_Old "Build for x86_64 (10.11)" OFF "APPLE;NOT PLATFORM_IOS" OFF)
cmake_dependent_option(MAC_IntelChip "Build for x86_64 (10.15)" OFF "APPLE;NOT PLATFORM_IOS" OFF)
cmake_dependent_option(MAC_AppleSilicon "Build for arm64 (11.0)" OFF "APPLE;NOT PLATFORM_IOS" OFF)
cmake_dependent_option(MAC_Universal "Support x86_64;arm64 at the same time" OFF "APPLE;NOT PLATFORM_IOS" OFF)
cmake_dependent_option(MAC_DOWNLOAD_DYLIB "Using precompiled dylib instead of system provided" OFF "APPLE;NOT PLATFORM_IOS" OFF)

# Linux
# Using CMAKE_INSTALL_PREFIX to specify the location where the application is installed.
cmake_dependent_option(INSTALL "Install to system path" OFF "UNIX;NOT APPLE" OFF)

# For Developer
option(DEBUG_SANITIZER "Turn on sanitizers (only available in debug build)" OFF)

# Google Analytics
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/GoogleAnalytics.cmake)

# Dependencies
option(USE_SHARED_LIB "Whether to use shared libs provided by system" OFF)
option(USE_SYSTEM_CURL "" ${USE_SHARED_LIB})
option(USE_SYSTEM_CPR "" ${USE_SHARED_LIB})
option(USE_SYSTEM_QRCODEGEN "" ${USE_SHARED_LIB})
option(USE_SYSTEM_PYSTRING "" ${USE_SHARED_LIB})
option(USE_SYSTEM_OPENCC "" ${USE_SHARED_LIB})
option(USE_SYSTEM_FMT "" ${USE_SHARED_LIB})
option(USE_SYSTEM_TINYXML2 "" ${USE_SHARED_LIB})
option(USE_SYSTEM_SDL2 "" ${USE_SHARED_LIB})
option(USE_SYSTEM_MONGOOSE "" ${USE_SHARED_LIB})
option(USE_SYSTEM_LUNASVG "" ${USE_SHARED_LIB})
option(USE_SYSTEM_GLFW "Not recommended because we used a modified GLFW" OFF)

option(DISABLE_OPENCC "Disable Chinese Simplified and Chinese Traditional Conversion" OFF)
option(DISABLE_WEBP "Disable webp" OFF)

# mpv related
# If your system does not support OpenGL(ES), you can use software rendering, but it will affect performance.
option(MPV_SW_RENDER "Using CPU to draw videos" OFF)
if (MPV_SW_RENDER)
    list(APPEND APP_PLATFORM_OPTION -DMPV_SW_RENDER)
endif ()

# On systems that do not support framebuffer, let MPV to draw to full screen and
# then cover unnecessary areas with UI.
option(MPV_NO_FB "Using system provided framebuffer" OFF)
if (MPV_NO_FB)
    list(APPEND APP_PLATFORM_OPTION -DMPV_NO_FB)
endif()

# Bundle mpv.dll into wiliwili.exe (Windows only)
cmake_dependent_option(MPV_BUNDLE_DLL "Bundle mpv.dll" OFF "USE_LIBROMFS;WIN32" OFF)
if (MPV_BUNDLE_DLL)
    list(APPEND APP_PLATFORM_OPTION -DMPV_BUNDLE_DLL)
    list(APPEND APP_PLATFORM_LIB MemoryModule)
endif ()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/extra.cmake)

# toolchain
include(${BOREALIS_LIBRARY}/cmake/toolchain.cmake)

# Git Info
git_info(GIT_TAG_VERSION GIT_TAG_SHORT)
list(APPEND APP_PLATFORM_OPTION -DBUILD_TAG_VERSION=${GIT_TAG_VERSION} -DBUILD_TAG_SHORT=${GIT_TAG_SHORT})

# nanovg
add_definitions(-DFONS_HASH_LUT_SIZE=4096)

# Project Info
project(wiliwili)
set(VERSION_MAJOR "1")
set(VERSION_MINOR "6")
set(VERSION_REVISION "0")
if (NOT VERSION_BUILD)
    set(VERSION_BUILD "0")
endif ()
set(PROJECT_AUTHOR "xfangfang")
set(PACKAGE_NAME cn.xfangfang.wiliwili)
set(VITA_TITLEID "WILIWILI0")
set(PSN_TITLE_ID "WILI00000")
set(VITA_VERSION "01.60")
set(PROJECT_ICON ${CMAKE_CURRENT_SOURCE_DIR}/resources/icon/icon.jpg)
set(PROJECT_RESOURCES ${CMAKE_CURRENT_SOURCE_DIR}/resources)
set(APP_VERSION "${VERSION_MAJOR}.${VERSION_MINOR}.${VERSION_REVISION}")
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_CURRENT_SOURCE_DIR}/cmake/")

if (PLATFORM_XBOX360)
    # OXDK supplies thread primitives through the XDK/runtime; there is no
    # host pthreads archive to pass to oxdklink.
else ()
    find_package(Threads REQUIRED)
    list(APPEND APP_PLATFORM_LIB ${CMAKE_THREAD_LIBS_INIT})
endif ()

# Find libmpv and libwebp
if (MAC_DOWNLOAD_DYLIB)
    include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/macos.cmake)
    list(APPEND CMAKE_MODULE_PATH
            ${CMAKE_BINARY_DIR}/deps/lib/cmake)
elseif (PLATFORM_DESKTOP)
    find_package(MPV REQUIRED)
    message(STATUS "Found libmpv: ${MPV_VERSION} ${MPV_INCLUDE_DIR} ${MPV_LIBRARY}")
    list(APPEND APP_PLATFORM_INCLUDE ${MPV_INCLUDE_DIR})
    if (NOT MPV_BUNDLE_DLL)
        list(APPEND APP_PLATFORM_LIB ${MPV_LIBRARY})
    endif ()

    find_package(WebP)
    if (WebP_FOUND AND NOT DISABLE_WEBP)
        message(STATUS "Found webp: ${WebP_INCLUDE_DIRS} ${WebP_LIBRARY}")
        list(APPEND APP_PLATFORM_OPTION -DUSE_WEBP)
        list(APPEND APP_PLATFORM_INCLUDE ${WebP_INCLUDE_DIRS})
        list(APPEND APP_PLATFORM_LIB ${WebP_LIBRARY})
    endif ()

    if (MINGW)
        set(CMAKE_EXE_LINKER_FLAGS "-static-libgcc -static-libstdc++ -static" CACHE STRING "" FORCE)
    endif ()
elseif (PLATFORM_XBOX360)
    # Xbox 360 uses the planned FFmpeg/custom transport path. Do not probe
    # desktop libmpv or pkg-config; XDK libraries are supplied by the toolchain.
    message(STATUS "Xbox 360 build: skipping libmpv/pkg-config dependency probing")
    # CPR headers remain part of the public HTTP API while its implementation
    # is supplied by the later bili360 transport slice.
    # curl/mbedtls are vendored from bili360-main into third_party/; the
    # BILI360_DIR environment variable is kept as a fallback for checkouts
    # where third_party/ has not been populated.
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/include/curl/curl.h")
        set(XBOX360_CURL_INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/include")
    else ()
        set(XBOX360_CURL_INCLUDE "$ENV{BILI360_DIR}/third_party/curl/include")
    endif ()
    list(APPEND APP_PLATFORM_INCLUDE
        ${CMAKE_CURRENT_SOURCE_DIR}/library/cpr/include
        ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/library/cmake/xbox360-cshim
        ${XBOX360_CURL_INCLUDE})

    set(XBOX360_BUILD_MBEDTLS ON CACHE BOOL "" FORCE)
    set(XBOX360_BUILD_CURL ON CACHE BOOL "" FORCE)
    set(XBOX360_BILI360_DIR "${CMAKE_CURRENT_SOURCE_DIR}" CACHE PATH "" FORCE)
    set(XBOX360_BUILD_CPR OFF CACHE BOOL "" FORCE)
    # curl itself is built with the mbedTLS backend; CPR only needs to expose
    # the generic Session API and must not run host SSL feature detection.
    set(XBOX360_ENABLE_SSL OFF CACHE BOOL "" FORCE)
    set(XBOX360_LIBCXX_ARCHIVE "${CMAKE_BINARY_DIR}/libcxx-xenon.a")
    list(APPEND APP_PLATFORM_LIB "${XBOX360_LIBCXX_ARCHIVE}" xbox360_platform_shims
        "${CMAKE_BINARY_DIR}/library/libcpr.a")

    # 运行时诊断探针总开关（选项在 library/borealis/library/cmake/toolchain.cmake
    # 里定义，默认 OFF）。关闭时 [x360-perf]/[x360-mpv] 等探针输出编译为空，
    # 不再刷屏；需要回归采集时用 -DXBOX360_RUNTIME_PROBES=ON 打开。
    if (XBOX360_RUNTIME_PROBES)
        list(APPEND APP_PLATFORM_OPTION -DX360_RUNTIME_PROBES=1)
    else ()
        list(APPEND APP_PLATFORM_OPTION -DX360_RUNTIME_PROBES=0)
    endif ()

    # 实验性 XAV 硬件解码路径（选项在 toolchain.cmake，默认 OFF，见
    # docs/XBOX360_OPTIMIZATION.md O10）。关闭时 mpv_core.cpp 的 XAV 分支整体
    # 不编译，构建与行为同历史版本。
    if (XBOX360_XAV)
        list(APPEND APP_PLATFORM_OPTION -DX360_XAV=1)
    else ()
        list(APPEND APP_PLATFORM_OPTION -DX360_XAV=0)
    endif ()

    # ---- libmpv + FFmpeg（cbffmpeg P4 第 4 步产物）--------------------------
    # 归档与顺序照 cbffmpeg 仓 tests/p4_libmpv/Makefile /
    # ../cbffmpeg/docs/wiliwili_mpv_embed.md §4（该文档不在本仓库 docs/ 下）：
    # oxdklink 把 `--` 之后的条目排在 --libs 的 XDK 归档之前，依赖方向从左到右
    # （前项引用后项），所以 compat 归档必须排在 libmpv/FFmpeg/libass/libxml2
    # 后面。缺任何一档说明 P4 还没跑完，直接中止而不是链出一个缺符号的 XEX。
    set(CBFFMPEG_DIR "$ENV{CBFFMPEG_DIR}")
    if (NOT CBFFMPEG_DIR)
        set(CBFFMPEG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../cbffmpeg")
    endif ()
    set(FFMPEG_BUILD_DIR "$ENV{FFMPEG_BUILD_DIR}")
    if (NOT FFMPEG_BUILD_DIR)
        set(FFMPEG_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/build/ffmpeg-x360-tls")
    endif ()
    set(MPV_BUILD_DIR "$ENV{MPV_BUILD_DIR}")
    if (NOT MPV_BUILD_DIR)
        set(MPV_BUILD_DIR "${CBFFMPEG_DIR}/build/mpv-x360-p4")
    endif ()
    set(XBOX360_LIBMPV_ARCHIVES
        "${MPV_BUILD_DIR}/libmpv.a"
        "${FFMPEG_BUILD_DIR}/libavfilter/libavfilter.a"
        "${FFMPEG_BUILD_DIR}/libswscale/libswscale.a"
        "${FFMPEG_BUILD_DIR}/libswresample/libswresample.a"
        "${FFMPEG_BUILD_DIR}/libavformat/libavformat.a"
        "${FFMPEG_BUILD_DIR}/libavcodec/libavcodec.a"
        "${FFMPEG_BUILD_DIR}/libavutil/libavutil.a"
        "${CBFFMPEG_DIR}/build/libass-x360/lib/libass.a"
        "${CBFFMPEG_DIR}/build/libass-x360/lib/libharfbuzz.a"
        "${CBFFMPEG_DIR}/build/libass-x360/lib/libfribidi.a"
        "${CBFFMPEG_DIR}/build/libass-x360/lib/libfreetype.a"
        "${CBFFMPEG_DIR}/build/libxml2-x360/lib/libxml2.a"
        "${CBFFMPEG_DIR}/build/compat-x360/libmpv_xbox360_compat.a"
        # 浮点分类/运算缺符号只在 compat 归档里没有对应成员，必须直接进链接
        "${CBFFMPEG_DIR}/compat/xbox360/xbox_float_math.o")
    foreach (_mpv_archive IN LISTS XBOX360_LIBMPV_ARCHIVES)
        if (NOT EXISTS "${_mpv_archive}")
            message(FATAL_ERROR
                "missing cbffmpeg P4 archive: ${_mpv_archive}\n"
                "build it first (../cbffmpeg/docs/wiliwili_mpv_embed.md §4), or point "
                "CBFFMPEG_DIR at the cbffmpeg checkout")
        endif ()
    endforeach ()
    list(APPEND APP_PLATFORM_INCLUDE "${MPV_BUILD_DIR}/include")
    list(APPEND APP_PLATFORM_LIB ${XBOX360_LIBMPV_ARCHIVES})

    # Xenon CRT gaps (C99 float/string helpers, MSVC intrinsics) and the console
    # stubs for symbols only the desktop CRT defines. See the source for why each
    # one is needed; nothing here stands in for a missing feature.
    add_library(xbox360_platform_shims STATIC
        ${CMAKE_CURRENT_SOURCE_DIR}/wiliwili/source/platform/xbox360/xbox360_crt_shims.c
        ${CMAKE_CURRENT_SOURCE_DIR}/wiliwili/source/platform/xbox360/xbox360_filesystem.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/wiliwili/source/platform/xbox360/xbox360_m3_shims.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/wiliwili/source/platform/xbox360/xbox360_network_runtime.cpp
        ${CBFFMPEG_DIR}/compat/xbox360/xbox_winsock_extra.c)
    target_include_directories(xbox360_platform_shims PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/library/cpr/include
        ${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/library/cmake/xbox360-cshim
        ${XBOX360_CURL_INCLUDE}
        ${CMAKE_CURRENT_SOURCE_DIR}/wiliwili/include)
    target_compile_definitions(xbox360_platform_shims PRIVATE NONET)
    set_source_files_properties(
        ${CBFFMPEG_DIR}/compat/xbox360/xbox_winsock_extra.c
        PROPERTIES COMPILE_OPTIONS "-I${CBFFMPEG_DIR}/compat/xbox360/include")
    # Real Itanium-ABI RTTI (__dynamic_cast + __cxxabiv1 typeinfo vtables) from
    # the libc++abi sources that ship with the OXDK llvm tree. The placeholders
    # that used to be stubbed here made every BRLS_BIND resolve() fail at
    # runtime. OXDK's builtins.c provides zeroed dummy vtables for four of the
    # typeinfo classes; oxdklink_xbox360.py weakens those during staging so the
    # real definitions below win.
    set(XBOX360_LIBCXXABI_DIR "$ENV{OXDK_LLVM}/../libcxxabi")
    if (NOT EXISTS "${XBOX360_LIBCXXABI_DIR}/src/private_typeinfo.cpp")
        message(FATAL_ERROR "libc++abi sources not found at ${XBOX360_LIBCXXABI_DIR}; set OXDK_LLVM to the OXDK llvm build directory")
    endif ()
    target_sources(xbox360_platform_shims PRIVATE "${XBOX360_LIBCXXABI_DIR}/src/private_typeinfo.cpp")
    # libc++abi's typeinfo objects follow the ordinary 32-bit Itanium layout;
    # compiling this implementation with the Xenon vtable-layout extension
    # makes it read base_type at the wrong offset from compiler-emitted RTTI.
    set_source_files_properties("${XBOX360_LIBCXXABI_DIR}/src/private_typeinfo.cpp"
        PROPERTIES COMPILE_OPTIONS "-Xclang;-target-feature;-Xclang;-xenon-abi")
    target_include_directories(xbox360_platform_shims SYSTEM PRIVATE
        "${XBOX360_LIBCXXABI_DIR}/include"
        "${XBOX360_LIBCXXABI_DIR}/src")
    target_compile_definitions(xbox360_platform_shims PRIVATE _LIBCXXABI_BUILDING_LIBRARY)
    # libc++'s compiled half (iostreams, locale, streambuf, stdexcept) has no prebuilt
    # library for Xenon; OXDK builds one from libc++'s own sources when
    # OXDK360_LIBCXX_LIB=1 and scripts/build_xbox360_libcxx.sh mirrors that setup.
    add_custom_target(xbox360_libcxx
        COMMAND ${CMAKE_COMMAND} -E env
            "BUILD=${CMAKE_BINARY_DIR}"
            "OXDK_DIR=$ENV{OXDK_DIR}"
            "XDK_DIR=$ENV{XDK_DIR}"
            "OXDK_LLVM=$ENV{OXDK_LLVM}"
            "LLVM_PREFIX=$ENV{OXDK_LLVM}"
            bash "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build_xbox360_libcxx.sh"
        BYPRODUCTS "${XBOX360_LIBCXX_ARCHIVE}"
        COMMENT "Building OXDK libc++ (libcxx-xenon.a) for Xenon"
        VERBATIM)
elseif (APP_PLATFORM_CUSTOM_LIBS)
    message(STATUS "Using custom libraries for mpv and webp")
    # If you want to manually specify dependencies:
    # cmake -B build -DPLATFORM_PSV \
    #   -DAPP_PLATFORM_CUSTOM_LIBS=ON \
    #   -DAPP_PLATFORM_INCLUDE="/opt/vita/include" \
    #   -DAPP_PLATFORM_LINK_OPTION="-L/opt/vita/lib -lmpv -lwebp"
else ()
    # Used for Cross Compile such as NintendoSwitch
    # If dependent files cannot be found, environment variables can be set before executing the cmake command:
    # PKG_CONFIG_LIBDIR=/some/path/arm64/lib/pkgconfig
    find_package(PkgConfig REQUIRED)

    pkg_search_module(MPV REQUIRED mpv)
    message(STATUS "Found libmpv: ${MPV_VERSION} ${MPV_INCLUDE_DIRS} ${MPV_STATIC_LIBRARIES}")
    list(APPEND APP_PLATFORM_INCLUDE ${MPV_INCLUDE_DIRS})
    list(APPEND APP_PLATFORM_LIB ${MPV_STATIC_LIBRARIES})
    link_directories(${MPV_LIBRARY_DIRS})
    if (PLATFORM_IOS)
        list(APPEND APP_PLATFORM_LIB "-framework CoreMedia -framework CoreText -framework VideoToolbox")
    endif ()

    pkg_search_module(LIBWEBP QUIET libwebp)
    if (LIBWEBP_FOUND AND NOT DISABLE_WEBP)
        message(STATUS "Found libwebp: ${LIBWEBP_INCLUDE_DIRS} ${LIBWEBP_LIBRARIES}")
        list(APPEND APP_PLATFORM_INCLUDE ${LIBWEBP_INCLUDE_DIRS})
        list(APPEND APP_PLATFORM_LIB ${LIBWEBP_LIBRARIES})
        list(APPEND APP_PLATFORM_OPTION -DUSE_WEBP)
        link_directories(${LIBWEBP_LIBRARY_DIRS})
    endif ()
endif ()

list(APPEND APP_PLATFORM_OPTION
   -DBUILD_PACKAGE_NAME=${PACKAGE_NAME}
   -DBUILD_VERSION_MAJOR=${VERSION_MAJOR}
   -DBUILD_VERSION_MINOR=${VERSION_MINOR}
   -DBUILD_VERSION_REVISION=${VERSION_REVISION}
)

if (BUILTIN_NSP)
    list(APPEND APP_PLATFORM_LIB nsp)
    list(APPEND APP_PLATFORM_OPTION -DBUILTIN_NSP)
    add_subdirectory(scripts/switch-forwarder)
endif ()

if (USE_LIBROMFS)
    add_libromfs(${PROJECT_NAME} ${PROJECT_RESOURCES})
endif ()

# set resources dir
if (CUSTOM_RESOURCES_DIR)
    set(BRLS_RESOURCES_DIR ${CUSTOM_RESOURCES_DIR})
elseif (INSTALL)
    set(BRLS_RESOURCES_DIR ${CMAKE_INSTALL_PREFIX}/share/wiliwili)
else ()
    set(BRLS_RESOURCES_DIR ".")
endif ()

# source cpp
file(GLOB_RECURSE MAIN_SRC wiliwili/source/*.cpp)
# 这三个文件只属于 X360 专属的 xbox360_platform_shims 静态库（XTL/Win32 API），
# 任何平台都不能进 MAIN_SRC：X360 下会与 shims 库重复，Desktop/Switch 下直接
# 编不过（xtl.h / windows.h 不存在）。
list(FILTER MAIN_SRC EXCLUDE REGEX "/platform/xbox360/xbox360_(m3_shims|filesystem|network_runtime)\\.cpp$")
if (PLATFORM_XBOX360)
    # libmpv 与 FFmpeg 六静态库由 cbffmpeg P4 产出（见
    # ../cbffmpeg/docs/wiliwili_mpv_embed.md §2/§4）：这里编真正的 mpv_core.cpp（走
    # MPV_SW_RENDER 软渲染），只把与播放链无关的 DLNA 单元排除在本轮编译外。
    # mpv_core.cpp 由上面的 GLOB 自动收进来；mpv_core_xbox360.cpp 是上一轮
    # 「libmpv 还没接」时的空壳实现，同样在 GLOB 里 —— 两边都编就是
    # MPVCore 全套成员函数 duplicate symbol，必须显式摘掉。
    list(REMOVE_ITEM MAIN_SRC
        ${CMAKE_CURRENT_SOURCE_DIR}/wiliwili/source/view/mpv_core_xbox360.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/wiliwili/source/activity/dlna_activity.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/wiliwili/source/api/dlna.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/wiliwili/source/fragment/player_dlna_search.cpp)
endif ()
if (WIN32)
    configure_file("${CMAKE_SOURCE_DIR}/wiliwili/app_win32.rc.in" "${CMAKE_BINARY_DIR}/app_win32.rc")
    list(APPEND MAIN_SRC ${CMAKE_BINARY_DIR}/app_win32.rc)
elseif (PLATFORM_PSV)
    configure_file("${CMAKE_SOURCE_DIR}/wiliwili/app_vita.xml.in" "${CMAKE_BINARY_DIR}/template.xml")
elseif (PLATFORM_SWITCH)
    # System service initialization functions required for libnx
    list(APPEND MAIN_SRC ${BOREALIS_LIBRARY}/lib/platforms/switch/switch_wrapper.c)
elseif (PLATFORM_PS4)
    # Show backtrace when app crash
    list(APPEND MAIN_SRC ${BOREALIS_LIBRARY}/lib/platforms/ps4/crashlog.c)
endif ()

# build borealis qrcode and other third party libraries
add_subdirectory(library)

# Main Target
program_target(${PROJECT_NAME} "${MAIN_SRC}")
if (PLATFORM_XBOX360 AND TARGET cpr)
    add_dependencies(${PROJECT_NAME} cpr)
endif ()
set_target_properties(${PROJECT_NAME} PROPERTIES
        CXX_STANDARD 17
        UNITY_BUILD ${BRLS_UNITY_BUILD}
)
set_source_files_properties(
        wiliwili/source/utils/config_helper.cpp
        wiliwili/source/view/mpv_core.cpp
        wiliwili/source/activity/setting_activity.cpp
        wiliwili/source/activity/dlna_activity.cpp
        PROPERTIES SKIP_UNITY_BUILD_INCLUSION ON)

# building release file
if (PLATFORM_DESKTOP)
    # Copy resources to build dir
    if (NOT USE_LIBROMFS)
        add_custom_target(${PROJECT_NAME}.data
            COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
            COMMAND "echo" "copy resources"
        )
        add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}.data)
    endif ()

    if (APPLE)
        if (MAC_DOWNLOAD_DYLIB)
            set(bundle_args1 "-nb")
            set(bundle_args2 "${CMAKE_BINARY_DIR}/deps/lib")
        else ()
            # Use system provided dylib
            set(bundle_args1 "-b")
            set(bundle_args2 "system")
        endif ()
        add_custom_target(
            ${PROJECT_NAME}.app
            COMMAND "bash" "${CMAKE_SOURCE_DIR}/scripts/build_mac.sh" "${bundle_args1}" "${bundle_args2}" "${APP_VERSION}"
        )
        add_dependencies(${PROJECT_NAME}.app ${PROJECT_NAME})
    elseif (UNIX AND INSTALL)
        set(icon_dirs 16x16 24x24 32x32 48x48 64x64 128x128 256x256)
        linux_install(
                "${PROJECT_RESOURCES}" "${BRLS_RESOURCES_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/resources/svg/${PACKAGE_NAME}.svg"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/linux/icons" "${icon_dirs}"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/linux/${PACKAGE_NAME}.appdata.xml"
                "${CMAKE_CURRENT_SOURCE_DIR}/scripts/linux/${PACKAGE_NAME}.desktop")
    endif ()
elseif (PLATFORM_IOS)
    ios_bundle(
            "${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/demo/ios/tvos/Splash.storyboard"
            "${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/demo/ios/iphoneos/Splash.storyboard"
            "${CMAKE_CURRENT_SOURCE_DIR}/library/borealis/demo/ios/Images.xcassets"
            "${CMAKE_CURRENT_SOURCE_DIR}/scripts/ios/iOSBundleInfo.plist.in"
            "wiliwili"
            "${APP_VERSION}")
elseif (PLATFORM_PSV)
    set(PSV_ASSETS_FILES
        ${CMAKE_SOURCE_DIR}/scripts/psv/sce_sys/icon0.png sce_sys/icon0.png
        ${CMAKE_SOURCE_DIR}/scripts/psv/sce_sys/pic0.png sce_sys/pic0.png
        ${CMAKE_SOURCE_DIR}/scripts/psv/sce_sys/bg.png sce_sys/livearea/contents/bg.png
        ${CMAKE_SOURCE_DIR}/scripts/psv/sce_sys/startup.png sce_sys/livearea/contents/startup.png
        ${CMAKE_BINARY_DIR}/template.xml sce_sys/livearea/contents/template.xml
        )
    if (NOT USE_LIBROMFS)
        list(APPEND PSV_ASSETS_FILES ${CMAKE_SOURCE_DIR}/resources resources)
    endif ()
    if (NOT USE_GXM)
        list(APPEND PSV_ASSETS_FILES ${CMAKE_SOURCE_DIR}/scripts/psv/module/ module)
    endif ()
    if (USE_VITA_SHARK)
        list(APPEND PSV_ASSETS_FILES "${CMAKE_BINARY_DIR}/vendor/SceShaccCg" module)
    endif ()
    list(APPEND APP_PLATFORM_LIB SceVshBridge_stub)
    set(VITA_MKSFOEX_FLAGS "${VITA_MKSFOEX_FLAGS} -d ATTRIBUTE2=12") # max heap size mode
    vita_create_self(${PROJECT_NAME}.self ${PROJECT_NAME} UNSAFE NOASLR)
    vita_create_vpk(${PROJECT_NAME}.vpk ${VITA_TITLEID} ${PROJECT_NAME}.self
            VERSION ${VITA_VERSION}
            NAME ${PROJECT_NAME}
            FILE ${PSV_ASSETS_FILES}
            )
elseif (PLATFORM_PS4)
    add_self(${PROJECT_NAME})
    add_pkg(${PROJECT_NAME}
            ${CMAKE_BINARY_DIR}/ps4
            ${PSN_TITLE_ID}
            "wiliwili"
            ${VITA_VERSION}
            8388608
    )
    add_custom_target(${PROJECT_NAME}.data
            COMMAND "${CMAKE_COMMAND}" -E copy_directory ${BOREALIS_DIR}/ps4/sce_module ${CMAKE_BINARY_DIR}/ps4/sce_module
            COMMAND "${CMAKE_COMMAND}" -E copy_directory ${CMAKE_SOURCE_DIR}/scripts/ps4/sce_sys ${CMAKE_BINARY_DIR}/ps4/sce_sys
            COMMAND "${CMAKE_COMMAND}" -E copy_directory ${BOREALIS_DIR}/ps4/sce_sys/about ${CMAKE_BINARY_DIR}/ps4/sce_sys/about
            COMMAND "${CMAKE_COMMAND}" -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/ps4/resources
            # Rename ja-RYU to RYU, as there may be a bug in PkgTool that not allow dirs with the same prefix exist.
            COMMAND "${CMAKE_COMMAND}" -E remove_directory ${CMAKE_BINARY_DIR}/ps4/resources/i18n/RYU
            COMMAND "${CMAKE_COMMAND}" -E rename ${CMAKE_BINARY_DIR}/ps4/resources/i18n/ja-RYU ${CMAKE_BINARY_DIR}/ps4/resources/i18n/RYU
    )
    add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}.data)
elseif (PLATFORM_SWITCH)
    set(BUILD_FONT_DIR ${CMAKE_BINARY_DIR}/resources/font)
    if (GIT_TAG_VERSION)
        string(SUBSTRING ${GIT_TAG_VERSION} 1 -1 APP_VERSION)
    endif ()
    if (BUILTIN_NSP)
        add_dependencies(${PROJECT_NAME} nsp_forwarder.nsp)
    endif ()
    if (USE_DEKO3D)
        gen_dksh("${PROJECT_RESOURCES}/shaders")
    endif ()
    add_custom_target(${PROJECT_NAME}.nro
        DEPENDS ${PROJECT_NAME}
        COMMAND ${NX_NACPTOOL_EXE} --create ${PROJECT_NAME} ${PROJECT_AUTHOR} ${APP_VERSION} ${PROJECT_NAME}.nacp
        COMMAND ${CMAKE_COMMAND} -E copy_directory ${PROJECT_RESOURCES} ${CMAKE_BINARY_DIR}/resources
        COMMAND ${CMAKE_COMMAND} -E remove -f ${BUILD_FONT_DIR}/*.txt ${BUILD_FONT_DIR}/switch_font.ttf
            ${BUILD_FONT_DIR}/keymap*.ttf
        COMMAND ${NX_ELF2NRO_EXE} ${PROJECT_NAME}.elf ${PROJECT_NAME}.nro --icon=${PROJECT_ICON}
            --nacp=${PROJECT_NAME}.nacp --romfsdir=${CMAKE_BINARY_DIR}/resources
        ALL
    )
endif ()

target_include_directories(${PROJECT_NAME} PRIVATE wiliwili/include wiliwili/include/api ${APP_PLATFORM_INCLUDE})
target_compile_options(${PROJECT_NAME} PRIVATE -ffunction-sections -fdata-sections -Wunused-variable ${APP_PLATFORM_OPTION})
if (PLATFORM_XBOX360)
    set(WILIWILI_ZLIB xbox360_zlib)
else ()
    set(WILIWILI_ZLIB z)
endif ()
target_link_libraries(${PROJECT_NAME} PRIVATE wiliwiliLibExtra borealis lunasvg pystring pdr mongoose ${WILIWILI_ZLIB} ${APP_PLATFORM_LIB})
if (PLATFORM_XBOX360)
    target_link_libraries(${PROJECT_NAME} PRIVATE xbox360_runtime)
    add_dependencies(${PROJECT_NAME} xbox360_libcxx)
endif ()
target_link_options(${PROJECT_NAME} PRIVATE ${APP_PLATFORM_LINK_OPTION})
