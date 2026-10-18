//
// Created by fang on 2022/8/12.
//

#include <cstdlib>
#include <clocale>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <pystring.h>
#include <borealis/core/thread.hpp>
#include <borealis/core/application.hpp>

#include "utils/config_helper.hpp"
#include "utils/number_helper.hpp"
#include "utils/crash_helper.hpp"
#include "view/mpv_core.hpp"

#if defined(PLATFORM_XBOX360)
extern "C" void xbox360_pthread_set_bind_spread(int enable);
#include <borealis/platforms/xbox360/xbox360_yuv.hpp>
#include <borealis/platforms/xbox360/xbox360_video.hpp>
#if X360_XAV
#include <borealis/platforms/xbox360/xbox360_xav.hpp>
#endif
#include <borealis/platforms/xbox360/xbox360_perf.hpp>

// Cheap breakdown of where a frame goes, so regressions can be attributed to
// decoding/conversion, texture upload or UI.  Logged every 60 samples.
//
// Timing is brls::xbox360perf (QueryPerformanceCounter): brls::getCPUTimeUsec()
// is only GetTickCount() on this platform and std::chrono::system_clock is
// ~1ms too, both of which made every sub-millisecond sample read back as zero.
namespace {
struct X360PerfProbe {
    double totalMs = 0;
    int samples    = 0;
    std::uint64_t lastFlush = 0;
    brls::xbox360perf::Ring ring;
    void add(double ms) {
        totalMs += ms;
        samples++;
        ring.add((float)ms);
    }
    void flush(const char *name) {
#if !X360_RUNTIME_PROBES
        // 探针关闭：只累计不输出（add() 仍记账，几乎零成本）。
        (void)name;
        return;
#endif
        if (samples == 0) return;
        // Flush every 2s of wall time as well as every 60 samples: at 2 fps
        // waiting for 60 samples meant the number never showed up during a
        // short playback window, which is exactly when it is needed.
        const std::uint64_t now = brls::xbox360perf::nowUsec();
        if (lastFlush == 0) {
            lastFlush = now;
            return;
        }
        if (now - lastFlush < 2000000 && samples < 60) return;
        brls::Logger::info("[x360-perf] {}: {:.2f} ms avg, p95 {:.2f}, max {:.2f} over {} samples", name,
                           totalMs / samples, ring.quantile(0.95f), ring.maxMs(), samples);
        totalMs   = 0;
        samples   = 0;
        lastFlush = now;
        ring.reset();
    }
};
X360PerfProbe x360_mpRender;   // mpvRenderContextRender: swscale conversion + plane clear
X360PerfProbe x360_draw;       // Xbox360YUVRenderer::draw on the UI thread

// setFrameSize counters (plan P0 / §9.3): distinguishes "the video rect was
// re-validated" from "the render surface really changed size".  A size-change
// rate close to the UI frame rate means the rect is oscillating every frame,
// which would re-run the synchronous mpv render (and possibly realloc) at
// frame rate.  Logged only on change (throttled) plus one summary line per
// window, never per frame.
struct X360SizeProbe {
    std::uint32_t calls   = 0;
    std::uint32_t changes = 0;
    std::uint32_t allocs  = 0;
    std::uint64_t lastFlush = 0;
    void flush(int w, int h, std::size_t pitch) {
#if !X360_RUNTIME_PROBES
        (void)w;
        (void)h;
        (void)pitch;
        return;
#endif
        if (calls == 0) return;
        const std::uint64_t now = brls::xbox360perf::nowUsec();
        if (lastFlush == 0) {
            lastFlush = now;
            return;
        }
        if (now - lastFlush < 2000000) return;
        brls::Logger::info(
            "[x360-perf] setFrameSize: {} calls, {} size changes, {} surface allocs, surface {}x{} pitch {}",
            calls, changes, allocs, w, h, pitch);
        calls   = 0;
        changes = 0;
        allocs  = 0;
        lastFlush = now;
    }
};
X360SizeProbe x360_size;

// Periodic mpv-side frame accounting (P0): separates "decoder/vo produced too
// few frames" from "render was slow".  getDouble/getInt read mpv properties
// synchronously; six reads every 2s is negligible next to the render cost.
struct X360MpvStatsProbe {
    std::uint64_t lastFlush = 0;
    void flush() {
#if !X360_RUNTIME_PROBES
        return;
#endif
        const std::uint64_t now = brls::xbox360perf::nowUsec();
        if (lastFlush == 0) {
            lastFlush = now;
            return;
        }
        if (now - lastFlush < 2000000) return;
        lastFlush = now;
        auto& core = MPVCore::instance();
        // rd = demuxer 拉流速度：配合 est-vf 崩塌 + cache 归零判定"网络饥饿"
        // （与主线程冻结是两种病：rd≈0 是拉不动流，主线程没卡也照样花屏音爆）。
        brls::Logger::info(
            "[x360-mpv] fps: container {:.2f} est-vf {:.2f} | drops: vo {} decoder {} vo-delayed {}"
            " | cache {:.1f}s rd {:.2f} MB/s",
            core.getDouble("container-fps"), core.getDouble("estimated-vf-fps"),
            core.getInt("frame-drop-count"), core.getInt("decoder-frame-drop-count"),
            core.getInt("vo-delayed-frame-count"), core.getDouble("demuxer-cache-duration"),
            core.getDouble("demuxer-cache-speed") / 1048576.0);
    }
};
X360MpvStatsProbe x360_mpvStats;

// Render-pipeline accounting (60fps output investigation): separates
// "mpv rarely signals an update" (vo_libmpv pacing) from "the main thread
// starves the render task" (sync-queue latency / coalescing).
// onUpdate/coalesced/tasksQueued/queueMs are written from mpv's internal
// thread; the rest only from the main thread (sync lambda + draw flush).
// Relaxed atomics: a report line may read one increment stale, harmless.
struct X360RenderPipeProbe {
    // mpv internal thread:
    std::atomic<std::uint32_t> onUpdate{0};    // update callback invoked
    std::atomic<std::uint32_t> coalesced{0};   // render already pending, merged
    std::atomic<std::uint32_t> tasksQueued{0}; // sync task submitted
    std::atomic<std::uint32_t> queueMs{0};     // enqueue time (ms), 0 = none
    // main thread:
    std::uint32_t tasksRun     = 0; // sync task executed
    std::uint32_t skipNoPixels = 0; // task ran but pixels not ready yet
    std::uint32_t updateFrame  = 0; // MPV_RENDER_UPDATE_FRAME seen
    std::uint32_t updateNone   = 0; // update without a new frame
    std::uint32_t renders      = 0; // mpvRenderContextRender called
    double latTotalMs          = 0; // queue latency (enqueue -> task start)
    double latMaxMs            = 0;
    std::uint32_t latCount     = 0;
    std::uint64_t lastFlush    = 0;

    void noteLatency(std::uint32_t enqMs) {
        if (enqMs == 0) return;
        // 32-bit ms wraps after ~49 days; unsigned delta stays correct for
        // any realistic queue latency.
        const double ms = (double)((std::uint32_t)(brls::xbox360perf::nowUsec() / 1000) - enqMs);
        latTotalMs += ms;
        latCount++;
        if (ms > latMaxMs) latMaxMs = ms;
    }

    void flush() {
#if !X360_RUNTIME_PROBES
        return;
#endif
        const std::uint64_t now = brls::xbox360perf::nowUsec();
        if (lastFlush == 0) {
            lastFlush = now;
            return;
        }
        if (now - lastFlush < 2000000) return;
        lastFlush = now;
        // 判读：update≈renders ≈ 片源帧率 → mpv 侧不缺帧；
        // update 高而 renders 低 → 主线程消费慢（看 queue 延迟）或 flags 缺帧；
        // update 本身就低 → vo_libmpv pacing（调 mpv 选项，与线程无关）。
        brls::Logger::info(
            "[x360-mpv] render-pipe: update {} coalesced {} queued {} run {} no-pix {} | "
            "flags frame/none {}/{} renders {} | queue {:.2f} ms avg, max {:.2f} (n {})",
            onUpdate.exchange(0), coalesced.exchange(0), tasksQueued.exchange(0),
            tasksRun, skipNoPixels, updateFrame, updateNone, renders,
            latCount ? latTotalMs / latCount : 0.0, latMaxMs, latCount);
        tasksRun = skipNoPixels = updateFrame = updateNone = renders = 0;
        latTotalMs = latMaxMs = 0;
        latCount              = 0;
    }
};
X360RenderPipeProbe x360_pipe;

inline double x360NowMs() {
    return (double)brls::xbox360perf::nowUsec() / 1000.0;
}
} // namespace
#endif

#ifdef MPV_BUNDLE_DLL
mpvSetOptionStringFunc mpvSetOptionString;
mpvObservePropertyFunc mpvObserveProperty;
mpvCreateFunc mpvCreate;
mpvInitializeFunc mpvInitialize;
mpvTerminateDestroyFunc mpvTerminateDestroy;
mpvSetWakeupCallbackFunc mpvSetWakeupCallback;
mpvCommandStringFunc mpvCommandString;
mpvErrorStringFunc mpvErrorString;
mpvWaitEventFunc mpvWaitEvent;
mpvGetPropertyFunc mpvGetProperty;
mpvCommandAsyncFunc mpvCommandAsync;
mpvGetPropertyStringFunc mpvGetPropertyString;
mpvFreeNodeContentsFunc mpvFreeNodeContents;
mpvSetOptionFunc mpvSetOption;
mpvFreeFunc mpvFree;
mpvRenderContextCreateFunc mpvRenderContextCreate;
mpvRenderContextSetUpdateCallbackFunc mpvRenderContextSetUpdateCallback;
mpvRenderContextRenderFunc mpvRenderContextRender;
mpvRenderContextReportSwapFunc mpvRenderContextReportSwap;
mpvRenderContextUpdateFunc mpvRenderContextUpdate;
mpvRenderContextFreeFunc mpvRenderContextFree;
mpvClientApiVersionFunc mpvClientApiVersion;
mpvRequestLogMessagesFunc mpvRequestLogMessages;
#endif

#ifdef MPV_USE_FB
#ifdef PS4
#include "utils/ps4_mpv_shaders.hpp"
#endif
#if defined(USE_GLES2)
#define SHADER_VERSION "#version 100\n"
#define SHADER_PRECISION "precision mediump float;\n"
#elif defined(USE_GLES3)
#define SHADER_VERSION "#version 300 es\n"
#define SHADER_PRECISION "precision mediump float;\n"
#else
#define SHADER_VERSION "#version 150 core\n"
#define SHADER_PRECISION ""
#endif

const char *vertexShaderSource = SHADER_VERSION
#ifdef USE_GLES2
    "attribute vec3 aPos;\n"
    "attribute vec2 aTexCoord;\n"
    "varying vec2 TexCoord;\n"
#else
    "in vec3 aPos;\n"
    "in vec2 aTexCoord;\n"
    "out vec2 TexCoord;\n"
#endif
    "void main()\n"
    "{\n"
    "   gl_Position = vec4(aPos, 1.0);\n"
    "   TexCoord = aTexCoord;\n"
    "}\0";
const char *fragmentShaderSource = SHADER_VERSION SHADER_PRECISION
#ifdef USE_GLES2
    "varying vec2 TexCoord;\n"
#else
    "in vec2 TexCoord;\n"
    "out vec4 FragColor;\n"
#endif
    "uniform sampler2D ourTexture;\n"
    "uniform float Alpha;\n"
    "void main()\n"
    "{\n"
#ifdef USE_GLES2
    "   gl_FragColor = texture2D(ourTexture, TexCoord);\n"
    "   gl_FragColor.a = Alpha;\n"
#else
    "   FragColor = texture(ourTexture, TexCoord);\n"
    "   FragColor.a = Alpha;\n"
#endif
    "}\n\0";

#define CONCATENATE3(s1, s2, s3) s1##s2##s3
#define CONCATENATE2(s1, s2) s1##s2
#define checkGLError(type, id)                                                 \
    {                                                                          \
        char *info = nullptr;                                                  \
        int length = 0;                                                        \
        CONCATENATE3(glGet, type, iv)(id, GL_COMPILE_STATUS, &length);         \
        if (length > 0) {                                                      \
            info = (char *)malloc(length);                                     \
            if (info) {                                                        \
                CONCATENATE3(glGet, type, InfoLog)(id, length, &length, info); \
            }                                                                  \
        }                                                                      \
        if (info) {                                                            \
            brls::Logger::error("Failed to load the shader: {}", info);        \
            free(info);                                                        \
        } else {                                                               \
            brls::Logger::error("Failed to load the shader");                  \
        }                                                                      \
        CONCATENATE2(glDelete, type)(id);                                      \
    }

static GLuint createShader(GLint type, const char *source) {
    GLuint shader = glCreateShader(type);
#ifdef PS4
    glShaderBinary(1, &shader, 2, type == GL_VERTEX_SHADER ? PS4_MPV_SHADER_VERT : PS4_MPV_SHADER_FRAG,
                   type == GL_VERTEX_SHADER ? PS4_MPV_SHADER_VERT_LENGTH : PS4_MPV_SHADER_FRAG_LENGTH);
#else
    int success;
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    // check for shader compile errors
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        checkGLError(Shader, shader);
        return 0;
    }
#endif
    return shader;
}

static GLuint linkProgram(GLuint s1, GLuint s2) {
    if (s1 == 0 || s2 == 0) return 0;
    GLuint program = glCreateProgram();
    int success;
    glAttachShader(program, s1);
    glAttachShader(program, s2);
    glLinkProgram(program);
    // check for linking errors
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        checkGLError(Program, program);
        return 0;
    }
    return program;
}
#endif

#ifdef BOREALIS_USE_DEKO3D
#include <borealis/platforms/switch/switch_video.hpp>
#elif defined(BOREALIS_USE_D3D11)
#include <borealis/platforms/driver/d3d11.hpp>
extern std::unique_ptr<brls::D3D11Context> D3D11_CONTEXT;
#elif defined(BOREALIS_USE_GXM)
#include <borealis/platforms/psv/psv_video.hpp>
#include <borealis/extern/nanovg/nanovg_gxm.h>
#elif defined(USE_GL2)
#undef glBindFramebuffer
#define glBindFramebuffer(a, b) void()
#endif

#if defined(__linux__) && defined(__GLFW__) && defined(USE_EGL)
#define GLFW_EXPOSE_NATIVE_X11
#define GLFW_EXPOSE_NATIVE_WAYLAND
#include <GLFW/glfw3native.h>
#endif

static inline void check_error(int status) {
    if (status < 0) {
        brls::Logger::error("MPV ERROR ====> {}", mpvErrorString(status));
    }
}

#if defined(BOREALIS_USE_OPENGL) && !defined(MPV_SW_RENDER)
static void *get_proc_address(void *unused, const char *name) {
#ifdef __SDL2__
    SDL_GL_GetCurrentContext();
    return (void *)SDL_GL_GetProcAddress(name);
#else
    glfwGetCurrentContext();
    return (void *)glfwGetProcAddress(name);
#endif
}
#endif

static inline float aspectConverter(const std::string &value) {
#if defined(__cpp_exceptions)
    try {
        if (value.empty()) {
            return -1;
        } else if (pystring::count(value, ":")) {
            // 比例模式
            auto num = pystring::split(value, ":");
            if (num.size() != 2) return -1;
            return std::stof(num[0]) / std::stof(num[1]);
        } else {
            // 纯数字
            return std::stof(value);
        }
    } catch (const std::exception &e) {
        return -1;
    }
#else
    // Xbox 360/OXDK 没有 unwinder：builtins.c 的 __cxa_throw 是 for(;;)，throw
    // 出去就再也回不来，所以 std::stof 换成不抛异常的 strtof。语义对齐 std::stof：
    // 跳过前导空白、允许尾部残渣（"12abc" → 12）、转换不了或 ERANGE → 返回失败。
    auto stofSafe = [](const std::string &s, float &out) -> bool {
        char *end    = nullptr;
        errno        = 0;
        out          = strtof(s.c_str(), &end);
        return end != s.c_str() && errno != ERANGE;
    };
    if (value.empty()) {
        return -1;
    } else if (pystring::count(value, ":")) {
        // 比例模式
        auto num = pystring::split(value, ":");
        if (num.size() != 2) return -1;
        float lhs = 0, rhs = 0;
        if (!stofSafe(num[0], lhs) || !stofSafe(num[1], rhs)) return -1;
        return lhs / rhs;
    } else {
        // 纯数字
        float parsed = 0;
        if (!stofSafe(value, parsed)) return -1;
        return parsed;
    }
#endif
}

void MPVCore::on_update(void *self) {
    auto *core = static_cast<MPVCore *>(self);
    if (!core) return;
#if defined(PLATFORM_XBOX360)
    x360_pipe.onUpdate.fetch_add(1, std::memory_order_relaxed);
#endif
#if defined(MPV_SW_RENDER)
    if (core->renderPending.exchange(true, std::memory_order_acq_rel)) {
#if defined(PLATFORM_XBOX360)
        x360_pipe.coalesced.fetch_add(1, std::memory_order_relaxed);
#endif
        return;
    }
#endif
#if defined(PLATFORM_XBOX360)
    x360_pipe.tasksQueued.fetch_add(1, std::memory_order_relaxed);
    x360_pipe.queueMs.store((std::uint32_t)(brls::xbox360perf::nowUsec() / 1000),
                            std::memory_order_relaxed);
#endif
    brls::sync([core]() {
#if defined(PLATFORM_XBOX360)
        x360_pipe.tasksRun++;
        x360_pipe.noteLatency(x360_pipe.queueMs.exchange(0, std::memory_order_relaxed));
#endif
#if defined(MPV_SW_RENDER)
        core->renderPending.store(false, std::memory_order_release);
#endif
        if (!core->mpv_context || !core->mpv) return;
        uint64_t flags = mpvRenderContextUpdate(core->mpv_context);
#if defined(PLATFORM_XBOX360)
        if (flags & MPV_RENDER_UPDATE_FRAME)
            x360_pipe.updateFrame++;
        else
            x360_pipe.updateNone++;
#endif
#if defined(MPV_NO_FB) || defined(BOREALIS_USE_DEKO3D) || defined(BOREALIS_USE_D3D11)
        // 直接绘制到屏幕上，需要屏幕每刷新一次绘制一次，在 MPVCore 的绘制函数内部处理
        (void)flags;
#else
        // 绘制到 FBO、纹理、buffer...上，在主循环结尾进行绘制，MPVCore 的绘制函数内直接绘制对应的 FBO、纹理、buffer...
        core->redraw = flags & MPV_RENDER_UPDATE_FRAME;
        if (core->redraw) {
#ifdef MPV_SW_RENDER
            if (!core->pixels) {
#if defined(PLATFORM_XBOX360)
                x360_pipe.skipNoPixels++;
#endif
                return;
            }
#if defined(PLATFORM_XBOX360)
            const double probeStart = x360NowMs();
            x360_pipe.renders++;
#endif
            int renderResult = mpvRenderContextRender(core->mpv_context, core->mpv_params);
#if defined(PLATFORM_XBOX360)
            x360_mpRender.add(x360NowMs() - probeStart);
            x360_mpRender.flush("mpv render call (swscale+clear)");
#endif
            if (renderResult < 0) {
                brls::Logger::error("mpv software render failed: {}", renderResult);
                core->redraw = false;
                return;
            }
            mpvRenderContextReportSwap(core->mpv_context);
            core->redraw = true;
#else
            mpvRenderContextRender(core->mpv_context, core->mpv_params);
#ifdef BOREALIS_USE_OPENGL
            glBindFramebuffer(GL_FRAMEBUFFER, core->default_framebuffer);
            glViewport(0, 0, (GLsizei)brls::Application::windowWidth, (GLsizei)brls::Application::windowHeight);
#endif
            mpvRenderContextReportSwap(core->mpv_context);
#endif
        }
#endif
    }, "mpv-rend");
}

void MPVCore::on_wakeup(void *self) {
    // tag "mpv-wk"：[stall-task] 看门狗 + 慢任务日志都能认出事件循环任务
    brls::sync([]() { MPVCore::instance().eventMainLoop(); }, "mpv-wk");
}

#if defined(MPV_BUNDLE_DLL)
template <typename Module, typename fnGetProcAddress>
void initMpvProc(Module dll, fnGetProcAddress pGetProcAddress) {
    mpvSetOptionString     = (mpvSetOptionStringFunc)pGetProcAddress(dll, "mpv_set_option_string");
    mpvObserveProperty     = (mpvObservePropertyFunc)pGetProcAddress(dll, "mpv_observe_property");
    mpvCreate              = (mpvCreateFunc)pGetProcAddress(dll, "mpv_create");
    mpvInitialize          = (mpvInitializeFunc)pGetProcAddress(dll, "mpv_initialize");
    mpvTerminateDestroy    = (mpvTerminateDestroyFunc)pGetProcAddress(dll, "mpv_terminate_destroy");
    mpvSetWakeupCallback   = (mpvSetWakeupCallbackFunc)pGetProcAddress(dll, "mpv_set_wakeup_callback");
    mpvCommandString       = (mpvCommandStringFunc)pGetProcAddress(dll, "mpv_command_string");
    mpvErrorString         = (mpvErrorStringFunc)pGetProcAddress(dll, "mpv_error_string");
    mpvWaitEvent           = (mpvWaitEventFunc)pGetProcAddress(dll, "mpv_wait_event");
    mpvGetProperty         = (mpvGetPropertyFunc)pGetProcAddress(dll, "mpv_get_property");
    mpvCommandAsync        = (mpvCommandAsyncFunc)pGetProcAddress(dll, "mpv_command_async");
    mpvGetPropertyString   = (mpvGetPropertyStringFunc)pGetProcAddress(dll, "mpv_get_property_string");
    mpvFreeNodeContents    = (mpvFreeNodeContentsFunc)pGetProcAddress(dll, "mpv_free_node_contents");
    mpvSetOption           = (mpvSetOptionFunc)pGetProcAddress(dll, "mpv_set_option");
    mpvFree                = (mpvFreeFunc)pGetProcAddress(dll, "mpv_free");
    mpvRenderContextCreate = (mpvRenderContextCreateFunc)pGetProcAddress(dll, "mpv_render_context_create");
    mpvRenderContextUpdate = (mpvRenderContextUpdateFunc)pGetProcAddress(dll, "mpv_render_context_update");
    mpvRenderContextFree   = (mpvRenderContextFreeFunc)pGetProcAddress(dll, "mpv_render_context_free");
    mpvRenderContextRender = (mpvRenderContextRenderFunc)pGetProcAddress(dll, "mpv_render_context_render");
    mpvRenderContextSetUpdateCallback =
        (mpvRenderContextSetUpdateCallbackFunc)pGetProcAddress(dll, "mpv_render_context_set_update_callback");
    mpvRenderContextReportSwap = (mpvRenderContextReportSwapFunc)pGetProcAddress(dll, "mpv_render_context_report_swap");
    mpvClientApiVersion        = (mpvClientApiVersionFunc)pGetProcAddress(dll, "mpv_client_api_version");
    mpvRequestLogMessages      = (mpvRequestLogMessagesFunc)pGetProcAddress(dll, "mpv_request_log_messages");
}
#endif

MPVCore::MPVCore() {
#if defined(MPV_BUNDLE_DLL)
    HMODULE hMpv = ::LoadLibraryW(L"libmpv-2.dll");
    if (!hMpv) {
        HRSRC hSrc   = ::FindResource(nullptr, "MPV", RT_RCDATA);
        HGLOBAL hRes = ::LoadResource(nullptr, hSrc);
        DWORD dwSize = ::SizeofResource(nullptr, hSrc);
        dll          = MemoryLoadLibrary(::LockResource(hRes), dwSize);
        ::FreeResource(hRes);

        brls::Logger::info("Load bundled libmpv-2.dll, size: {}", dwSize);
        initMpvProc(dll, MemoryGetProcAddress);
    } else {
        char dllPath[MAX_PATH];
        ::GetModuleFileNameA(hMpv, dllPath, sizeof(dllPath));

        brls::Logger::info("Load external `{}`", dllPath);
        initMpvProc(hMpv, GetProcAddress);
    }
#endif
    this->init();
    // Destroy mpv when application exit
    brls::Application::getExitDoneEvent()->subscribe([this]() {
        this->clean();
#if defined(PLATFORM_XBOX360) && X360_XAV
        // XAV must be shut down before the D3D device is destroyed.
        brls::Xbox360Xav::instance().shutdown();
#endif
#ifdef MPV_SW_RENDER
        if (pixels_storage) {
            free(pixels_storage);
            pixels_storage = nullptr;
            pixels         = nullptr;
            pixels_bytes   = 0;
            mpv_params[3].data = nullptr;
        }
#endif
    });
}

void MPVCore::init() {
#if defined(PLATFORM_XBOX360)
    // Spread FFmpeg/mpv pthread workers across the three Xenon cores before
    // mpv starts creating decoder threads.
    xbox360_pthread_set_bind_spread(1);
#endif
    setlocale(LC_NUMERIC, "C");
    this->mpv = mpvCreate();
    if (!mpv) {
        brls::fatal("Error Create mpv Handle");
    }
    std::string confDir = ProgramConfig::instance().getConfigDir();
    // misc
    mpvSetOptionString(mpv, "config", "yes");
    mpvSetOptionString(mpv, "config-dir", confDir.c_str());
    mpvSetOptionString(mpv, "gpu-shader-cache-dir", fmt::format("{}/cache", confDir).c_str());
    mpvSetOptionString(mpv, "ytdl", "no");
    mpvSetOptionString(mpv, "audio-channels", "stereo");
    mpvSetOptionString(mpv, "idle", "yes");
    mpvSetOptionString(mpv, "loop-file", "no");
    mpvSetOptionString(mpv, "osd-level", "0");
    mpvSetOptionString(mpv, "video-timing-offset", "0");  // 60fps
    mpvSetOptionString(mpv, "keep-open", "yes");
    mpvSetOptionString(mpv, "hr-seek", "yes");
    mpvSetOptionString(mpv, "reset-on-next-file", "speed,pause");
    mpvSetOptionString(mpv, "vo", "libmpv");
    mpvSetOptionString(mpv, "pulse-latency-hacks", "no");

    mpvSetOption(mpv, "brightness", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_BRIGHTNESS);
    mpvSetOption(mpv, "contrast", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_CONTRAST);
    mpvSetOption(mpv, "saturation", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_SATURATION);
    mpvSetOption(mpv, "hue", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_HUE);
    mpvSetOption(mpv, "gamma", MPV_FORMAT_DOUBLE, &MPVCore::VIDEO_GAMMA);

    if (MPVCore::LOW_QUALITY) {
        // Less cpu cost
        brls::Logger::info("lavc: skip loop filter and set fast decode");
        mpvSetOptionString(mpv, "vd-lavc-skiploopfilter", "all");
        mpvSetOptionString(mpv, "vd-lavc-fast", "yes");
        if (mpvClientApiVersion() >= MPV_MAKE_VERSION(2, 2)) {
            mpvSetOptionString(mpv, "profile", "fast");
        } else {
            mpvSetOptionString(mpv, "scale", "bilinear");
            mpvSetOptionString(mpv, "dscale", "bilinear");
            mpvSetOptionString(mpv, "dither", "no");
            mpvSetOptionString(mpv, "correct-downscaling", "no");
            mpvSetOptionString(mpv, "linear-downscaling", "no");
            mpvSetOptionString(mpv, "sigmoid-upscaling", "no");
            mpvSetOptionString(mpv, "hdr-compute-peak", "no");
            mpvSetOptionString(mpv, "allow-delayed-peak-detect", "yes");
        }
    }

    if (MPVCore::INMEMORY_CACHE) {
        // cache
        brls::Logger::info("set memory cache: {}MB", MPVCore::INMEMORY_CACHE);
        mpvSetOptionString(mpv, "demuxer-max-bytes", fmt::format("{}MiB", MPVCore::INMEMORY_CACHE).c_str());
        mpvSetOptionString(mpv, "demuxer-max-back-bytes", fmt::format("{}MiB", MPVCore::INMEMORY_CACHE / 2).c_str());
    } else {
        mpvSetOptionString(mpv, "cache", "no");
    }

    // hardware decoding
    if (HARDWARE_DEC) {
        mpvSetOptionString(mpv, "hwdec", PLAYER_HWDEC_METHOD.c_str());
        brls::Logger::info("MPV hardware decode: {}", PLAYER_HWDEC_METHOD);
    } else {
        mpvSetOptionString(mpv, "hwdec", "no");
    }

    // Making the loading process faster
#if defined(__SWITCH__)
    mpvSetOptionString(mpv, "vd-lavc-dr", "no");
    mpvSetOptionString(mpv, "vd-lavc-threads", "4");
    // This should fix random crash, but I don't know why.
    mpvSetOptionString(mpv, "opengl-glfinish", "yes");
#elif defined(PS4)
    mpvSetOptionString(mpv, "vd-lavc-threads", "6");
#elif defined(__PSV__)
    mpvSetOptionString(mpv, "vd-lavc-threads", "4");
    mpvSetOptionString(mpv, "fbo-format", "rgba8");

    // Fix vo_wait_frame() cannot be wakeup
    mpvSetOptionString(mpv, "video-latency-hacks", "yes");
#elif defined(PLATFORM_XBOX360)
    // Six logical hardware threads are available; cbffmpeg's pthread shim
    // spreads the workers across physical cores and then SMT siblings.
    mpvSetOptionString(mpv, "vd-lavc-threads", "6");
    // NOTE: "framedrop=decoder+vo" was tried here and reverted: decoder-side
    // NONREF skipping engaged (decoder drops ~30/s, est-vf 60->31) but delay
    // still drifted +0.1~0.16 s/s, VO drop-gate kept the client at ~5.5Hz,
    // and 720p30 playback became visibly janky. Do not re-enable without a
    // new plan.
    // P0 round 2 (XBOX360_PLAYBACK_PERFORMANCE_PLAN.md §12): mpv's sws
    // defaults are LANCZOS + full-chroma HQ flags, and this ffmpeg build has
    // no AltiVec (HAVE_ALTIVEC=0), so the scalar C resampler costs ~440ms per
    // windowed 1280x720 -> 800x450 frame. The 1:1 fullscreen case takes
    // swscale's unscaled copy path and is unaffected either way. Area (box)
    // downscale with fast flags; log the return codes so an option-name
    // mismatch is visible on the console.
    int swsScaler = mpvSetOptionString(mpv, "sws-scaler", "area");
    int swsFast   = mpvSetOptionString(mpv, "sws-fast", "yes");
    X360_PROBE_INFO("[x360-sw] sws-scaler=area -> {}, sws-fast=yes -> {}", swsScaler, swsFast);
#endif
    // 过低的值可能导致部分直播流无法正确播放
    mpvSetOptionString(mpv, "demuxer-lavf-analyzeduration", "0.4");
    mpvSetOptionString(mpv, "demuxer-lavf-probescore", "24");

    // log
    // mpvSetOptionString(mpv, "msg-level", "ffmpeg=trace");
    // mpvSetOptionString(mpv, "msg-level", "all=no");
    if (MPVCore::TERMINAL) {
        mpvSetOptionString(mpv, "terminal", "yes");
        if (brls::Logger::getLogLevel() >= brls::LogLevel::LOG_DEBUG) {
            mpvSetOptionString(mpv, "msg-level", "all=v");
        }
    }

    if (mpvInitialize(mpv) < 0) {
        mpvTerminateDestroy(mpv);
        brls::fatal("Could not initialize mpv context");
    }

#if defined(PLATFORM_XBOX360)
#if X360_RUNTIME_PROBES
    // P0 diagnostics: without this call mpv never emits MPV_EVENT_LOG_MESSAGE,
    // so the [x360-sw] probe inside the software render path (reinit/clear/
    // scale + src->dst geometry) and mpv's own notices, e.g. the swscale
    // slow-copy warning (mp_verbose, M4), never reach the console log.
    mpvRequestLogMessages(mpv, "v");
#else
    // 探针关闭：只保留 warn/error 级 mpv 日志。请求 "v" 会让 mpv 的 verbose
    // 消息（含每 30 帧一行的 [x360-sw] 探针）全部经 Logger 输出，是 XBDM
    // 日志刷屏的主要来源之一。
    mpvRequestLogMessages(mpv, "warn");
#endif
#endif

    // set observe properties
    check_error(mpvObserveProperty(mpv, 1, "core-idle", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 2, "eof-reached", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 3, "duration", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 4, "playback-time", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 5, "cache-speed", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 6, "percent-pos", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 7, "paused-for-cache", MPV_FORMAT_FLAG));
    //    check_error(mpvObserveProperty(mpv, 8, "demuxer-cache-time", MPV_FORMAT_DOUBLE));
    //    check_error(mpvObserveProperty(mpv, 9, "demuxer-cache-state", MPV_FORMAT_NODE));
    check_error(mpvObserveProperty(mpv, 10, "speed", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 11, "volume", MPV_FORMAT_INT64));
    check_error(mpvObserveProperty(mpv, 12, "pause", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 13, "playback-abort", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 14, "seeking", MPV_FORMAT_FLAG));
    check_error(mpvObserveProperty(mpv, 15, "hwdec-current", MPV_FORMAT_STRING));
    check_error(mpvObserveProperty(mpv, 16, "path", MPV_FORMAT_STRING));
    check_error(mpvObserveProperty(mpv, 17, "brightness", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 18, "contrast", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 19, "saturation", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 20, "gamma", MPV_FORMAT_DOUBLE));
    check_error(mpvObserveProperty(mpv, 21, "hue", MPV_FORMAT_DOUBLE));

    // init renderer params
#ifdef MPV_SW_RENDER
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_SW)},
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
#elif defined(BOREALIS_USE_DEKO3D)
    int advanced_control{1};
    auto switchPlatform = (brls::SwitchVideoContext *)brls::Application::getPlatform()->getVideoContext();
    mpv_deko3d_init_params deko_init_params{switchPlatform->getDeko3dDevice()};
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_DEKO3D)},
                              {MPV_RENDER_PARAM_DEKO3D_INIT_PARAMS, &deko_init_params},
                              {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
#elif defined(BOREALIS_USE_D3D11)
    mpv_dxgi_init_params init_params{D3D11_CONTEXT->getDevice(), D3D11_CONTEXT->getSwapChain()};
    mpv_render_param params[]{
        {MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_DXGI)},
        {MPV_RENDER_PARAM_DXGI_INIT_PARAMS, &init_params},
        {MPV_RENDER_PARAM_INVALID, nullptr},
    };
#elif defined(BOREALIS_USE_GXM)
    auto *video_context            = (brls::PsvVideoContext *)brls::Application::getPlatform()->getVideoContext();
    auto *window                   = video_context->getWindow();
    auto *vg                       = brls::Application::getNVGContext();
    mpv_gxm_init_params gxm_params = {
        .context        = window->context,
        .shader_patcher = window->shader_patcher,
        .buffer_index   = 0,
        .msaa           = SCE_GXM_MULTISAMPLE_4X,
    };

    mpv_render_param params[] = {{MPV_RENDER_PARAM_API_TYPE, (void *)MPV_RENDER_API_TYPE_GXM},
                                 {MPV_RENDER_PARAM_GXM_INIT_PARAMS, &gxm_params},
                                 {MPV_RENDER_PARAM_INVALID, nullptr}};

    if (mpv_fbo.render_target == nullptr) {
        int texture_width     = DISPLAY_WIDTH;
        int texture_height    = DISPLAY_HEIGHT;
        int texture_stride    = ALIGN(texture_width, 8);
        nvg_image             = nvgCreateImageRGBA(vg, texture_width, texture_height, 0, nullptr);
        NVGXMtexture *texture = nvgxmImageHandle(vg, nvg_image);

        NVGXMframebufferInitOptions framebufferOpts = {
            .display_buffer_count = 1,  // Must be 1 for custom FBOs
            .scenesPerFrame       = 1,
            .render_target        = texture,
            .color_format         = SCE_GXM_COLOR_FORMAT_U8U8U8U8_ABGR,
            .color_surface_type   = SCE_GXM_COLOR_SURFACE_LINEAR,
            .display_width        = texture_width,
            .display_height       = texture_height,
            .display_stride       = texture_stride,
        };
        NVGXMframebuffer *fbo = gxmCreateFramebuffer(&framebufferOpts);
        mpv_fbo.render_target = fbo->gxm_render_target;
        mpv_fbo.color_surface = &fbo->gxm_color_surfaces[0].surface;
        mpv_fbo.depth_stencil_surface = &fbo->gxm_depth_stencil_surface;
        mpv_fbo.w   = texture_width;
        mpv_fbo.h   = texture_height;
    }
#else
    int advanced_control{1};
    mpv_opengl_init_params gl_init_params{get_proc_address, nullptr};
    mpv_render_param params[]{{MPV_RENDER_PARAM_API_TYPE, const_cast<char *>(MPV_RENDER_API_TYPE_OPENGL)},
                              {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &gl_init_params},
                              {MPV_RENDER_PARAM_ADVANCED_CONTROL, &advanced_control},
#if defined(GLFW_EXPOSE_NATIVE_X11)
                              {MPV_RENDER_PARAM_X11_DISPLAY, glfwGetX11Display()},
#endif
#if defined(GLFW_EXPOSE_NATIVE_WAYLAND)
                              {MPV_RENDER_PARAM_WL_DISPLAY, glfwGetWaylandDisplay()},
#endif
                              {MPV_RENDER_PARAM_INVALID, nullptr}};
#endif

    if (mpvRenderContextCreate(&mpv_context, mpv, params) < 0) {
        mpvTerminateDestroy(mpv);
        brls::fatal("failed to initialize mpv GL context");
    }
#ifdef BOREALIS_USE_D3D11
    wiliwili::initCrashDump();
#endif
    brls::Logger::info("MPV Version: {}", mpvGetPropertyString(mpv, "mpv-version"));
    brls::Logger::info("FFMPEG Version: {}", mpvGetPropertyString(mpv, "ffmpeg-version"));
    command_async("set", "audio-client-name", APPVersion::getPackageName());
    setVolume(MPVCore::VIDEO_VOLUME);

    // set event callback
    mpvSetWakeupCallback(mpv, on_wakeup, this);
    // set render callback
    mpvRenderContextSetUpdateCallback(mpv_context, on_update, this);

    focusSubscription = brls::Application::getWindowFocusChangedEvent()->subscribe([this](bool focus) {
        static bool playing = false;
        static std::chrono::system_clock::time_point sleepTime{};
        if (focus) {
            // restore AUTO_PLAY
            AUTO_PLAY = ProgramConfig::instance().getBoolOption(SettingItem::PLAYER_AUTO_PLAY);
            // application is on top
            auto timeNow = std::chrono::system_clock::now();
            if (playing && timeNow < (sleepTime + std::chrono::seconds(120))) {
                resume();
            }
        } else {
            // application is sleep, save the current state
            playing   = isPlaying();
            sleepTime = std::chrono::system_clock::now();
            pause();
            // do not automatically play video
            AUTO_PLAY = false;
        }
    });

    brls::Application::getExitEvent()->subscribe([]() { disableDimming(false); });

    this->initializeVideo();
}

#ifdef MPV_BUNDLE_DLL
MPVCore::~MPVCore() { MemoryFreeLibrary(dll); }
#else
MPVCore::~MPVCore() = default;
#endif

void MPVCore::clean() {
#if defined(PLATFORM_XBOX360) && X360_XAV
    // Drop the experimental XAV player along with mpv; restart() rebuilds mpv
    // and XAV is re-initialized lazily on the next setUrl.
    brls::Xbox360Xav::instance().close();
#endif
    check_error(mpvCommandString(this->mpv, "quit"));

    brls::Application::getWindowFocusChangedEvent()->unsubscribe(focusSubscription);

    brls::Logger::info("uninitialize Video");
    this->uninitializeVideo();

    brls::Logger::info("trying free mpv context");
    if (this->mpv_context) {
        mpvRenderContextFree(this->mpv_context);
        this->mpv_context = nullptr;
    }

    brls::Logger::info("trying terminate mpv");
    if (this->mpv) {
        mpvTerminateDestroy(this->mpv);
        this->mpv = nullptr;
    }
}

void MPVCore::restart() {
    this->clean();
    this->init();
    setMirror(MPVCore::VIDEO_MIRROR);
    setShader(currentShaderProfile, currentShader, currentSetting, false);
    mpvCoreEvent.fire(MpvEventEnum::RESTART);

    // 如果正在播放视频时重启mpv，重启前后存在软硬解切，那么视频尺寸会不正确
    // 手动设置一次尺寸可以解决这个问题 (同 MPVCore::reset())
    setFrameSize(rect);
}

void MPVCore::uninitializeVideo() {
#ifdef MPV_USE_FB
    if (media_framebuffer != 0) glDeleteFramebuffers(1, &media_framebuffer);
    if (media_texture != 0) glDeleteTextures(1, &media_texture);
    if (shader.vbo != 0) glDeleteBuffers(1, &shader.vbo);
    if (shader.ebo != 0) glDeleteBuffers(1, &shader.ebo);
    if (shader.prog != 0) glDeleteProgram(shader.prog);
#ifdef MPV_USE_VAO
    if (shader.vao != 0) glDeleteVertexArrays(1, &shader.vao);
    shader.vao = 0;
#endif
    media_framebuffer = 0;
    media_texture     = 0;
    shader.vbo        = 0;
    shader.ebo        = 0;
    shader.prog       = 0;
#endif
}

void MPVCore::initializeVideo() {
#if defined(BOREALIS_USE_OPENGL) && !defined(MPV_SW_RENDER)
    // Get default framebuffer
#if defined(IOS)
    // SDL: OpenGL ES on iOS doesn't use the traditional system-framebuffer setup provided in other operating systems.
    default_framebuffer = 1;
#else
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &default_framebuffer);
#endif
#endif

#if defined(MPV_NO_FB)
    mpv_fbo.fbo = default_framebuffer;
    glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
#elif defined(MPV_USE_FB)
    if (this->media_texture != 0) return;
    brls::Logger::debug("initializeGL");

    // create texture
    glGenTextures(1, &this->media_texture);
    glBindTexture(GL_TEXTURE_2D, this->media_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (int)brls::Application::windowWidth, (int)brls::Application::windowHeight,
                 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // create frame buffer
    glGenFramebuffers(1, &this->media_framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, this->media_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, this->media_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        brls::Logger::error("glCheckFramebufferStatus failed");
        return;
    }

    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
    this->mpv_fbo.fbo = (int)this->media_framebuffer;
    brls::Logger::debug("create fbo and texture done\n");

    // build and compile our shader program
    // ------------------------------------
    // vertex shader
    GLuint vertexShader = createShader(GL_VERTEX_SHADER, vertexShaderSource);
    // fragment shader
    GLuint fragmentShader = createShader(GL_FRAGMENT_SHADER, fragmentShaderSource);

    // link shaders
    GLuint shaderProgram = linkProgram(vertexShader, fragmentShader);

    if (vertexShader) glDeleteShader(vertexShader);
    if (fragmentShader) glDeleteShader(fragmentShader);
    this->shader.prog = shaderProgram;

    // set up vertex data (and buffer(s)) and configure vertex attributes
    // ------------------------------------------------------------------
    unsigned int indices[] = {0, 1, 3, 1, 2, 3};
    glGenBuffers(1, &this->shader.vbo);
    glBindBuffer(GL_ARRAY_BUFFER, this->shader.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    glGenBuffers(1, &this->shader.ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->shader.ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, sizeof(indices), indices, GL_STATIC_DRAW);

#ifdef MPV_USE_VAO
    glGenVertexArrays(1, &this->shader.vao);
    glBindVertexArray(this->shader.vao);
    glBindBuffer(GL_ARRAY_BUFFER, this->shader.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, this->shader.ebo);

    GLuint aPos = glGetAttribLocation(shaderProgram, "aPos");
    glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)nullptr);
    glEnableVertexAttribArray(aPos);

    GLuint aTexCoord = glGetAttribLocation(shaderProgram, "aTexCoord");
    glVertexAttribPointer(aTexCoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(aTexCoord);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
#endif
    brls::Logger::debug("initializeGL done");
#endif
}

void MPVCore::setFrameSize(brls::Rect r) {
#if defined(MPV_SW_RENDER) && defined(PLATFORM_XBOX360)
    const brls::Rect prevRect = rect;
#endif
    rect = r;
    if (std::isnan(rect.getWidth()) || std::isnan(rect.getHeight())) return;

#ifdef MPV_SW_RENDER
    int drawWidth  = rect.getWidth() * brls::Application::windowScale;
    int drawHeight = rect.getHeight() * brls::Application::windowScale;
    if (drawWidth == 0 || drawHeight == 0) return;
#if defined(PLATFORM_XBOX360)
    // 1:1 surface (plan §12/§13): keep the surface at the video's native size
    // so libmpv_sw hits swscale's unscaled copy path (~2ms) instead of
    // resampling in scalar C (~90-120ms with area, ~440ms with mpv's default
    // LANCZOS; HAVE_ALTIVEC=0).  The surface always carries the video's exact
    // aspect, so mpv's fit is the full surface and the conversion is 1:1;
    // letterboxing for whatever aspect the view rect has is done by the GPU
    // quad instead (Xbox360YUVRenderer::draw fits the texture aspect into the
    // rect, and the NanoVG black background above fills the bars).  Buffer-
    // baked bars would force the CPU scale again — that is what made a
    // 854x480 live stream in an 800x477 window cost ~120ms/frame.  Before the
    // video's size is known, fall back to the rect-sized surface.
    const int videoW = getInt("video-params/w");
    const int videoH = getInt("video-params/h");
    const bool videoSizeKnown = videoW >= 16 && videoH >= 16 && (videoW % 2) == 0 && (videoH % 2) == 0;
    swSurfaceFromVideo = videoSizeKnown;
    if (videoSizeKnown) {
        drawWidth  = videoW;
        drawHeight = videoH;
    }
    // The surface stays even for 4:2:0 chroma; the ceiling is the D3D9 max
    // texture size (4096) clamped to bilibili's live 原画 maximum (4K).
    // PSV-parity decision (2026-10-04, plan §14.6): PSV renders through
    // mpv's GPU path — it uploads native-size frames and scales in the
    // shader, so it never freezes on a 1080p stream.  The Xbox360 equivalent
    // of "GPU scaling" is the YUV quad (Xbox360YUVRenderer::draw fits any
    // texture into the view rect), so the surface follows the video's native
    // size as well.  The old 1280x720 cap forced libmpv_sw to resample every
    // 1080p frame in scalar C (~250-360ms, HAVE_ALTIVEC=0) and pinned the
    // main thread to ~3fps; ~85% of live rooms offer only the original
    // quality tier (no transcode) and ignore qn=250, so 1080p live is
    // unavoidable.  Above the ceiling the keep-aspect resample below remains
    // as the last resort (no real content can reach it: VOD is client-capped
    // at 720p, live 原画 tops out at 4K).
    constexpr int maxRenderWidth  = 3840;
    constexpr int maxRenderHeight = 2160;
    if (videoSizeKnown) {
        if (drawWidth > maxRenderWidth || drawHeight > maxRenderHeight) {
            // Over the cap the surface must still keep the video's aspect
            // (an independent clamp would distort it and bring the CPU scale
            // back).  Only sources above 4K could land here — none exist in
            // practice (VOD is client-capped at 720p, live 原画 tops out at
            // 4K), so the resample path is dead code.
            double scale = 1.0;
            const double wScale = (double)maxRenderWidth / (double)drawWidth;
            const double hScale = (double)maxRenderHeight / (double)drawHeight;
            if (wScale < scale) scale = wScale;
            if (hScale < scale) scale = hScale;
            drawWidth  = (int)((double)drawWidth * scale);
            drawHeight = (int)((double)drawHeight * scale);
        }
    } else {
        if (drawWidth > maxRenderWidth) drawWidth = maxRenderWidth;
        if (drawHeight > maxRenderHeight) drawHeight = maxRenderHeight;
    }
    drawWidth &= ~1;
    drawHeight &= ~1;
    if (drawWidth == 0 || drawHeight == 0) return;
    // A pitch that is a multiple of 128 keeps the luma stride (and therefore
    // the chroma stride, half of it) 32-byte aligned, which is what swscale
    // requires before it will write into the buffer without an extra copy,
    // and additionally lands every row on the 128-byte Xenon cache line so
    // the per-frame plane upload copies whole lines (the frame pools in the
    // Xbox 360 YouTube client are likewise 128-byte aligned).
    const size_t newPitch  = ((size_t)drawWidth + 127) & ~(size_t)127;
    const size_t requiredBytes = newPitch * (size_t)drawHeight + newPitch * (size_t)((drawHeight + 1) / 2);
#else
    const size_t requiredBytes = (size_t)drawWidth * (size_t)drawHeight * PIXCEL_SIZE;
#endif

#if defined(PLATFORM_XBOX360)
    // P0 counters (plan §9.3): only a real surface size/pitch change runs the
    // synchronous mpv render below and may realloc.  Log on change only, and
    // throttle after the first few so a runaway rect loop cannot flood the
    // performance capture with per-frame lines.
    x360_size.calls++;
    if (sw_size[0] != drawWidth || sw_size[1] != drawHeight || pitch != newPitch) {
        x360_size.changes++;
        if (x360_size.changes <= 8 || (x360_size.changes % 64) == 0)
            brls::Logger::info(
                "MPVCore::setFrameSize #{}: surface {}/{} -> {}/{} pitch {} -> {} rect {:.0f}x{:.0f} -> {:.0f}x{:.0f}",
                x360_size.changes, sw_size[0], sw_size[1], drawWidth, drawHeight, (size_t)pitch, newPitch,
                prevRect.getWidth(), prevRect.getHeight(), rect.getWidth(), rect.getHeight());
        else
            brls::Logger::debug(
                "MPVCore::setFrameSize #{}: surface {}/{} -> {}/{} pitch {} -> {} rect {:.0f}x{:.0f} -> {:.0f}x{:.0f}",
                x360_size.changes, sw_size[0], sw_size[1], drawWidth, drawHeight, (size_t)pitch, newPitch,
                prevRect.getWidth(), prevRect.getHeight(), rect.getWidth(), rect.getHeight());
    }
#endif

    if (pixels_storage != nullptr && requiredBytes > pixels_bytes) {
        brls::Logger::debug("Enlarge video surface buffer");
        free(pixels_storage);
        pixels_storage = nullptr;
        pixels         = nullptr;
        pixels_bytes   = 0;
    }

    if (pixels == nullptr) {
#if defined(PLATFORM_XBOX360)
        // libswscale insists on 32-byte aligned plane pointers and strides;
        // align to the 128-byte Xenon cache line instead, which satisfies
        // swscale and keeps plane starts off cache-line middles (matching the
        // 128-byte aligned frame memory used by the Xbox 360 YouTube client).
        pixels_storage = malloc(requiredBytes + 127);
        if (pixels_storage == nullptr) return;
        pixels = (void *)(((uintptr_t)pixels_storage + 127) & ~(uintptr_t)127);
#else
        pixels_storage = malloc(requiredBytes);
        if (pixels_storage == nullptr) return;
        pixels = pixels_storage;
#endif
        pixels_bytes   = requiredBytes;
        mpv_params[3].data = pixels;
#if defined(PLATFORM_XBOX360)
        x360_size.allocs++;
        {
            // A freshly malloc'd surface is zeroed, and zeroed YUV renders
            // green through the shader.  Pre-fill limited-range black so a
            // failed/absent first mpv render can never show green.
            auto* base   = (unsigned char*)pixels;
            const size_t lumaPitch = (size_t)newPitch;
            const size_t lumaH     = (size_t)drawHeight;
            const size_t cw        = (lumaPitch + 1) / 2;
            const size_t ch        = (lumaH + 1) / 2;
            std::memset(base, 16, lumaPitch * lumaH);
            std::memset(base + lumaPitch * lumaH, 128, cw * ch);
            std::memset(base + lumaPitch * lumaH + cw * ch, 128, cw * ch);
        }
#endif
    }

    sw_size[0] = drawWidth;
    sw_size[1] = drawHeight;
#if defined(PLATFORM_XBOX360)
    pitch = newPitch;
    // No RGBA texture to rebuild: mpv writes straight into `pixels`.
    brls::Logger::debug("MPVCore::setFrameSize: {}/{} pitch={}", drawWidth, drawHeight, (size_t)pitch);
#else
#if defined(BOREALIS_USE_D3D11)
    // 使用 dx11 的拷贝交换，否则视频渲染异常
    const static int mpvImageFlags = NVG_IMAGE_STREAMING | NVG_IMAGE_COPY_SWAP;
#else
    const static int mpvImageFlags = 0;
#endif
    if (nvg_image) nvgDeleteImage(brls::Application::getNVGContext(), nvg_image);
    nvg_image = nvgCreateImageRGBA(brls::Application::getNVGContext(), drawWidth, drawHeight, mpvImageFlags,
                                   (const unsigned char *)pixels);
    pitch      = PIXCEL_SIZE * drawWidth;
#endif

    // 在视频暂停时调整纹理尺寸，视频画面会被清空为黑色，强制重新绘制一次，避免这个问题
    const int setFrameRenderResult = mpvRenderContextRender(mpv_context, mpv_params);
    if (setFrameRenderResult < 0)
        brls::Logger::error("MPVCore::setFrameSize: mpv software render failed: {}", setFrameRenderResult);
    mpvRenderContextReportSwap(mpv_context);
    // 上面直接渲染了一次，标记需要回写到 nvg 纹理（否则新纹理内容停在创建时的数据）
    this->redraw = true;
#elif !defined(MPV_USE_FB)
        // Using default framebuffer
#if defined(BOREALIS_USE_GXM)
    // This line will be called between beginFrame() and endFrame() in Application::frame(),
    // but mpvRenderContextRender(...) will call functions similar to beginFrame() and endFrame() to draw content to FBO,
    // and that will cause error in GXM, so call in brls::sync to make the mpv drawing calls outside the brls::Application::frame().
    brls::sync([this]() {
        mpvRenderContextRender(mpv_context, mpv_params);
        mpvRenderContextReportSwap(mpv_context);
    });
#elif defined(BOREALIS_USE_D3D11)
#else
    this->mpv_fbo.w = brls::Application::windowWidth;
    this->mpv_fbo.h = brls::Application::windowHeight;
#endif
    if (brls::Application::contentWidth < rect.getMaxX() || brls::Application::contentHeight < rect.getMaxY()) return;
    command_async("set", "video-margin-ratio-right",
                  (brls::Application::contentWidth - rect.getMaxX()) / brls::Application::contentWidth);
    command_async("set", "video-margin-ratio-bottom",
                  (brls::Application::contentHeight - rect.getMaxY()) / brls::Application::contentHeight);
    command_async("set", "video-margin-ratio-top", rect.getMinY() / brls::Application::contentHeight);
    command_async("set", "video-margin-ratio-left", rect.getMinX() / brls::Application::contentWidth);
#else
    if (this->media_texture == 0) return;
    int drawWidth  = rect.getWidth() * brls::Application::windowScale;
    int drawHeight = rect.getHeight() * brls::Application::windowScale;

    if (drawWidth == 0 || drawHeight == 0) return;
    brls::Logger::debug("MPVCore::setFrameSize: {}/{}", drawWidth, drawHeight);
    glBindTexture(GL_TEXTURE_2D, this->media_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, drawWidth, drawHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    this->mpv_fbo.w = drawWidth;
    this->mpv_fbo.h = drawHeight;

    float new_min_x = rect.getMinX() / brls::Application::contentWidth * 2 - 1;
    float new_min_y = 1 - rect.getMinY() / brls::Application::contentHeight * 2;
    float new_max_x = rect.getMaxX() / brls::Application::contentWidth * 2 - 1;
    float new_max_y = 1 - rect.getMaxY() / brls::Application::contentHeight * 2;

    vertices[0]  = new_max_x;
    vertices[1]  = new_min_y;
    vertices[5]  = new_max_x;
    vertices[6]  = new_max_y;
    vertices[10] = new_min_x;
    vertices[11] = new_max_y;
    vertices[15] = new_min_x;
    vertices[16] = new_min_y;

    glBindBuffer(GL_ARRAY_BUFFER, this->shader.vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);

    // 在视频暂停时调整纹理尺寸，视频画面会被清空为黑色，强制重新绘制一次，避免这个问题
    mpvRenderContextRender(mpv_context, mpv_params);
    glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
    glViewport(0, 0, (GLsizei)brls::Application::windowWidth, (GLsizei)brls::Application::windowHeight);
    mpvRenderContextReportSwap(mpv_context);
#endif
}

bool MPVCore::isValid() const { return mpv != nullptr && mpv_context != nullptr; }

#if defined(PLATFORM_XBOX360) && X360_XAV
namespace {
// --- Experimental Xbox 360 hardware decode (XAV) ---------------------------
// See library/borealis/.../xbox360_xav.hpp.  XAV decodes the video stream
// while mpv is reduced to audio only (vid=no).  Everything here is inert
// unless the user turned on Settings > Playback > Hardware decode.

bool xavActive() { return MPVCore::HARDWARE_DEC && brls::Xbox360Xav::instance().initialized(); }

// Pull audio-file="..." out of the mpv option string built by VideoView.
bool extractAudioOption(const std::string& extra, std::string& out) {
    const std::string key = "audio-file=\"";
    const auto pos        = extra.find(key);
    if (pos == std::string::npos) return false;
    const auto start = pos + key.size();
    const auto end   = extra.find('"', start);
    if (end == std::string::npos || end == start) return false;
    out = extra.substr(start, end - start);
    return true;
}

// Start (or restart) XAV on the given video URL.  Ignores mpv-only URLs.
void startXavVideo(const std::string& url) {
    if (url.empty() || url.rfind("edl://", 0) == 0) return;
    auto* videoContext = (brls::Xbox360VideoContext*)brls::Application::getPlatform()->getVideoContext();
    if (!videoContext || !videoContext->getDevice()) return;
    auto& xav = brls::Xbox360Xav::instance();
    if (!xav.init(videoContext->getDevice(), videoContext->getPresentParameters())) return;
    if (!xav.open(url))
        brls::Logger::error("XAV could not open the video stream (Optional Media Update codec pack missing?)");
}

// Video-only draw for the XAV path: black background + the decoded texture.
void drawXavFrame(const brls::Rect& rect, float alpha) {
    auto* vg = brls::Application::getNVGContext();
    if (!vg) return;

    nvgBeginPath(vg);
    NVGcolor bg{};
    bg.a = alpha;
    nvgFillColor(vg, bg);
    nvgRect(vg, rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
    nvgFill(vg);

    auto& xav = brls::Xbox360Xav::instance();
    int w = 0, h = 0;
    IDirect3DTexture9* tex = xav.acquireFrame(w, h);
    if (tex && w > 0 && h > 0)
        brls::Xbox360YUVRenderer::drawTexture(vg, tex, w, h, rect.getMinX(), rect.getMinY(), rect.getWidth(),
                                              rect.getHeight(), alpha);
    xav.releaseFrame();
}
} // namespace
#endif

void MPVCore::draw(brls::Rect area, float alpha) {
    if (mpv_context == nullptr) return;
#if defined(PLATFORM_XBOX360) && X360_XAV
    if (xavActive()) {
        // Experimental hardware decode: the picture comes from XAV, mpv only
        // feeds the audio clock.
        this->rect = area;
        drawXavFrame(this->rect, alpha);
        return;
    }
#endif
    if (!(this->rect == area)) setFrameSize(area);
#if defined(MPV_SW_RENDER) && defined(PLATFORM_XBOX360)
    // The video's native size can change without the rect changing (first
    // frame after load, quality switch).  Re-run setFrameSize when it does so
    // the 1:1 surface engages; sampled every 250ms because getInt is a
    // synchronous mpv property read.
    {
        static std::uint64_t nextVideoCheck = 0;
        static int lastVideoW = 0, lastVideoH = 0;
        const std::uint64_t now = brls::xbox360perf::nowUsec();
        if (now >= nextVideoCheck) {
            nextVideoCheck = now + 250000;
            const int vw = getInt("video-params/w");
            const int vh = getInt("video-params/h");
            // Also record the cleared state (0) while switching videos: only
            // remembering the last *known* size suppressed this re-run for a
            // next video of the same resolution (both default to 640x360),
            // leaving the surface on the rect fallback and the scalar scale
            // running until a rect change (fullscreen) forced setFrameSize.
            // When params are known but the surface still derives from the
            // rect (fallback engaged while params were unavailable), force a
            // re-run as well so the 1:1 surface engages.
            if (vw != lastVideoW || vh != lastVideoH || (vw > 0 && !swSurfaceFromVideo)) {
                lastVideoW = vw;
                lastVideoH = vh;
                if (vw > 0 && vh > 0)
                    setFrameSize(area);
            }
        }
    }
#endif

#ifdef MPV_SW_RENDER
    if (!pixels) return;

    auto *vg = brls::Application::getNVGContext();

    // draw black background: covers the whole video area so anything mpv did
    // not paint shows as black instead of stale UI
    nvgBeginPath(vg);
    NVGcolor bg{};
    bg.a = alpha;
    nvgFillColor(vg, bg);
    nvgRect(vg, rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
    nvgFill(vg);

#if defined(PLATFORM_XBOX360)
    // Upload only when mpv produced a new frame; otherwise the L8 textures
    // from the previous upload are reused.
    const bool freshFrame = this->redraw;
    this->redraw          = false;

    const double probeStart = x360NowMs();
    // Xbox360YUVRenderer flushes the NanoVG geometry queued so far (the
    // background above and every widget drawn before the video), uploads the
    // planes and lets the GPU convert YUV -> RGB.  Whatever is drawn after
    // this call is flushed on top of the video when Application::frame ends
    // the NanoVG frame, so the normal draw order is preserved.
    brls::Xbox360YUVRenderer::draw(vg, freshFrame ? pixels : nullptr, sw_size[0], sw_size[1], pitch, rect.getMinX(),
                                   rect.getMinY(), rect.getWidth(), rect.getHeight(), alpha);
    x360_draw.add(x360NowMs() - probeStart);
    x360_draw.flush("yuv renderer call (flush+upload+draw)");
    x360_size.flush(sw_size[0], sw_size[1], pitch);
    x360_mpvStats.flush();
    x360_pipe.flush();
#else
    // Only upload a new MPV frame; the render callback marks this flag.
    if (this->redraw) {
        this->redraw = false;
        nvgUpdateImage(vg, nvg_image, (const unsigned char *)pixels);
    }

    // draw video
    nvgBeginPath(vg);
    nvgRect(vg, rect.getMinX(), rect.getMinY(), rect.getWidth(), rect.getHeight());
    nvgFillPaint(vg, nvgImagePattern(vg, 0, 0, rect.getWidth(), rect.getHeight(), 0, nvg_image, alpha));
    nvgFill(vg);
#endif
#elif defined(BOREALIS_USE_GXM)
    auto *vg = brls::Application::getNVGContext();

    if (MPVCore::VIDEO_MIRROR) {
        nvgSave(vg);
        nvgTranslate(vg, area.getWidth() + area.getMinX() * 2, 0);
        nvgScale(vg, -1, 1);
    }
    NVGpaint img = nvgImagePattern(vg, 0, 0, brls::Application::contentWidth, brls::Application::contentHeight, 0,
                                   nvg_image, alpha);
    nvgBeginPath(vg);
    nvgRect(vg, area.getMinX(), area.getMinY(), area.getWidth(), area.getHeight());
    nvgFillPaint(vg, img);
    nvgFill(vg);
    if (MPVCore::VIDEO_MIRROR) {
        nvgRestore(vg);
    }
#elif defined(MPV_NO_FB) || defined(BOREALIS_USE_DEKO3D) || defined(BOREALIS_USE_D3D11)
    // 只在非透明时绘制视频，可以避免退出页面时视频画面残留
    if (alpha >= 1) {
#ifdef BOREALIS_USE_DEKO3D
        static auto videoContext = (brls::SwitchVideoContext *)brls::Application::getPlatform()->getVideoContext();
        mpv_fbo.tex              = videoContext->getFramebuffer();
        videoContext->queueSignalFence(&readyFence);
        videoContext->queueFlush();
#endif
        mpvRenderContextRender(this->mpv_context, mpv_params);
#ifdef BOREALIS_USE_DEKO3D
        videoContext->queueWaitFence(&doneFence);
#elif defined(BOREALIS_USE_D3D11)
        D3D11_CONTEXT->beginFrame();
#else
        glBindFramebuffer(GL_FRAMEBUFFER, default_framebuffer);
        glViewport(0, 0, brls::Application::windowWidth, brls::Application::windowHeight);
#endif
        mpvRenderContextReportSwap(this->mpv_context);

        // 画背景来覆盖mpv的黑色边框
        if (rect.getWidth() < brls::Application::contentWidth) {
            auto *vg = brls::Application::getNVGContext();
            nvgBeginPath(vg);
            nvgFillColor(vg, brls::Application::getTheme().getColor("brls/background"));
            nvgRect(vg, 0, 0, rect.getMinX(), brls::Application::contentHeight);
            nvgRect(vg, rect.getMaxX(), 0, brls::Application::contentWidth - rect.getMaxX(),
                    brls::Application::contentHeight);
            nvgRect(vg, rect.getMinX() - 1, 0, rect.getWidth() + 2, rect.getMinY());
            nvgRect(vg, rect.getMinX() - 1, rect.getMaxY(), rect.getWidth() + 2,
                    brls::Application::contentHeight - rect.getMaxY());
            nvgFill(vg);
        }
    }
#else
    // OpenGL 将 FBO 绘制到屏幕上
    glUseProgram(shader.prog);
    glBindTexture(GL_TEXTURE_2D, this->media_texture);
#ifdef MPV_USE_VAO
    glBindVertexArray(shader.vao);
#else
    glBindBuffer(GL_ARRAY_BUFFER, shader.vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, shader.ebo);
    static GLuint aPos = glGetAttribLocation(shader.prog, "aPos");
    glVertexAttribPointer(aPos, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)nullptr);
    glEnableVertexAttribArray(aPos);

    static GLuint aTexCoord = glGetAttribLocation(shader.prog, "aTexCoord");
    glVertexAttribPointer(aTexCoord, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glEnableVertexAttribArray(aTexCoord);
#endif

    // Set alpha
    if (alpha < 1) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }
    static GLint alphaID = glGetUniformLocation(shader.prog, "Alpha");
    glUniform1f(alphaID, alpha);

    glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_INT, nullptr);
#endif
}

mpv_render_context *MPVCore::getContext() { return this->mpv_context; }

mpv_handle *MPVCore::getHandle() { return this->mpv; }

MPVEvent *MPVCore::getEvent() { return &this->mpvCoreEvent; }

std::string MPVCore::getCacheSpeed() const {
    if (cache_speed >> 20 > 0) {
        return fmt::format("{:.2f} MB/s", (cache_speed >> 10) / 1024.0f);
    } else if (cache_speed >> 10 > 0) {
        return fmt::format("{:.2f} KB/s", cache_speed / 1024.0f);
    } else {
        return fmt::format("{} B/s", cache_speed);
    }
}

void MPVCore::eventMainLoop() {
    while (true) {
        auto event = mpvWaitEvent(this->mpv, 0);
#if defined(PLATFORM_XBOX360)
        {
            // 阶段细化到事件 id：事件循环整体卡住时看门狗直接报出卡在
            // 哪类事件的处理里（evt1=SHUTDOWN evt8=FILE_LOADED evt17=LOG...）。
            char evtStage[24];
            std::snprintf(evtStage, sizeof(evtStage), "mpv:evt%d", (int)event->event_id);
            brls::xbox360perf::stageMark(evtStage);
        }
#endif
        switch (event->event_id) {
            case MPV_EVENT_NONE:
                return;
            case MPV_EVENT_LOG_MESSAGE: {
                auto log = (mpv_event_log_message *)event->data;
                if (log->log_level <= MPV_LOG_LEVEL_ERROR) {
                    brls::Logger::error("{}: {}", log->prefix, log->text);
                } else if (log->log_level <= MPV_LOG_LEVEL_WARN) {
                    brls::Logger::warning("{}: {}", log->prefix, log->text);
                } else if (log->log_level <= MPV_LOG_LEVEL_INFO) {
                    brls::Logger::info("{}: {}", log->prefix, log->text);
                } else if (log->log_level <= MPV_LOG_LEVEL_V) {
#if defined(PLATFORM_XBOX360)
                    // The swscale slow-copy notice is mp_verbose; promote it to
                    // info so M4 stays visible at the release log level while
                    // every other verbose line keeps its debug severity.
                    if (log->text && std::string(log->text).find("slow copy") != std::string::npos) {
                        brls::Logger::info("{}: {}", log->prefix, log->text);
                    } else {
                        brls::Logger::debug("{}: {}", log->prefix, log->text);
                    }
#else
                    brls::Logger::debug("{}: {}", log->prefix, log->text);
#endif
                } else {
                    brls::Logger::verbose("{}: {}", log->prefix, log->text);
                }
            } break;
            case MPV_EVENT_FILE_LOADED:
                brls::Logger::info("========> MPV_EVENT_FILE_LOADED");
                // event 8: 文件预加载结束，准备解码
                mpvCoreEvent.fire(MpvEventEnum::MPV_LOADED);
                // 发布一次进度更新事件，避免进度条在0秒时没有进度更新
                video_progress = 0;
                mpvCoreEvent.fire(MpvEventEnum::UPDATE_PROGRESS);
                // 移除其他备用链接
                command_async("playlist-clear");

                if (AUTO_PLAY) {
                    mpvCoreEvent.fire(MpvEventEnum::MPV_RESUME);
                    this->resume();
                } else {
                    mpvCoreEvent.fire(MpvEventEnum::MPV_PAUSE);
                    this->pause();
                }
                break;
            case MPV_EVENT_START_FILE:
                // event 6: 开始加载文件
                brls::Logger::info("========> MPV_EVENT_START_FILE");

                // show osd for a really long time
                mpvCoreEvent.fire(MpvEventEnum::START_FILE);

                mpvCoreEvent.fire(MpvEventEnum::LOADING_START);
                break;
            case MPV_EVENT_PLAYBACK_RESTART:
                // event 21: 开始播放文件（一般是播放或调整进度结束之后触发）
                brls::Logger::info("========> MPV_EVENT_PLAYBACK_RESTART");
                video_stopped = false;
                mpvCoreEvent.fire(MpvEventEnum::LOADING_END);
                break;
            case MPV_EVENT_END_FILE: {
                // event 7: 文件播放结束
                brls::Logger::info("========> MPV_STOP");
                mpvCoreEvent.fire(MpvEventEnum::MPV_STOP);
                video_stopped = true;
                auto node     = (mpv_event_end_file *)event->data;
                if (node->reason == MPV_END_FILE_REASON_ERROR) {
                    mpv_error_code = node->error;
                    brls::Logger::error("========> MPV ERROR: {}", mpvErrorString(node->error));
                    mpvCoreEvent.fire(MpvEventEnum::MPV_FILE_ERROR);
                }
#ifdef BOREALIS_USE_GXM
                else
                {
                    setFrameSize(rect); // 清空残留画面
                }
#endif

                break;
            }
            case MPV_EVENT_PROPERTY_CHANGE: {
                auto *data = ((mpv_event_property *)event->data)->data;
                switch (event->reply_userdata) {
                    case 1:
                        if (data) {
                            bool playing = *(int *)data == 0;
                            if (playing != video_playing) {
                                video_playing = playing;
                                mpvCoreEvent.fire(MpvEventEnum::MPV_IDLE);
                            }
                            video_playing = playing;
                            disableDimming(video_playing);
                        }
                        break;
                    case 2:
                        if (data) video_eof = *(int *)data;
                        // 当视频播放自然结束时会先触发一次 EOF，如果这时 stop 或者设置了新的播放文件，会触发 STOP 然后再触发一次 EOF
                        // 避免多次触发 EOF，将第二种情况排除在外
                        if (video_eof && !video_stopped) {
                            brls::Logger::info("========> END OF FILE");
                            mpvCoreEvent.fire(MpvEventEnum::END_OF_FILE);
                        }
                        break;
                    case 3:
                        // 视频总时长更新
                        if (((mpv_event_property *)event->data)->data)
                            duration = *(int64_t *)((mpv_event_property *)event->data)->data;
                        if (duration != 0) {
                            brls::Logger::debug("========> duration: {}", duration);
                            mpvCoreEvent.fire(MpvEventEnum::UPDATE_DURATION);
                        }
                        break;
                    case 4:
                        // 播放进度更新
                        if (((mpv_event_property *)event->data)->data) {
                            playback_time = *(double *)((mpv_event_property *)event->data)->data;
                            if (video_progress != (int64_t)playback_time) {
                                video_progress = (int64_t)playback_time;
                                mpvCoreEvent.fire(MpvEventEnum::UPDATE_PROGRESS);
                                // 判断是否需要暂停播放
                                if (CLOSE_TIME > 0 && wiliwili::getUnixTime() > CLOSE_TIME) {
                                    CLOSE_TIME = 0;
                                    this->pause();
                                }
                            }
                        }

                        break;
                    case 5:
                        // 视频 cache speed
                        if (((mpv_event_property *)event->data)->data) {
                            cache_speed = *(int64_t *)((mpv_event_property *)event->data)->data;
                            mpvCoreEvent.fire(MpvEventEnum::CACHE_SPEED_CHANGE);
                        }
                        break;
                    case 6:
                        // 视频进度更新（百分比）
                        if (((mpv_event_property *)event->data)->data) {
                            percent_pos = *(double *)((mpv_event_property *)event->data)->data;
                        }
                        break;
                    case 7:
                        // 发生了缓存等待
                        if (!data) break;

                        if (*(int *)data) {
                            brls::Logger::info("========> VIDEO PAUSED FOR CACHE");
                            mpvCoreEvent.fire(MpvEventEnum::LOADING_START);
                        } else {
                            brls::Logger::info("========> VIDEO RESUME FROM CACHE");
                            mpvCoreEvent.fire(MpvEventEnum::LOADING_END);
                        }
                        break;
                    case 8:
                        // 缓存时间
                        if (((mpv_event_property *)event->data)->data) {
                            brls::Logger::verbose("demuxer-cache-time: {}",
                                                  *(double *)((mpv_event_property *)event->data)->data);
                        }
                        break;
                    case 9:
                        // 缓存信息
                        if (((mpv_event_property *)event->data)->data) {
                            auto *node = (mpv_node *)((mpv_event_property *)event->data)->data;
                            std::unordered_map<std::string, mpv_node> node_map;
                            for (int i = 0; i < node->u.list->num; i++) {
                                node_map.insert(
                                    std::make_pair(std::string(node->u.list->keys[i]), node->u.list->values[i]));
                            }
                            brls::Logger::debug(
                                "total-bytes: {:.2f}MB; cache-duration: "
                                "{:.2f}; "
                                "underrun: {}; fw-bytes: {:.2f}MB; bof-cached: "
                                "{}; eof-cached: {}; file-cache-bytes: {}; "
                                "raw-input-rate: {:.2f};",
                                node_map["total-bytes"].u.int64 / 1048576.0, node_map["cache-duration"].u.double_,
                                node_map["underrun"].u.flag, node_map["fw-bytes"].u.int64 / 1048576.0,
                                node_map["bof-cached"].u.flag, node_map["eof-cached"].u.flag,
                                node_map["file-cache-bytes"].u.int64 / 1048576.0,
                                node_map["raw-input-rate"].u.int64 / 1048576.0);
                        }
                        break;
                    case 10:
                        // 倍速信息
                        if (data) {
                            video_speed = *(double *)data;
                            mpvCoreEvent.fire(VIDEO_SPEED_CHANGE);
                        }
                        break;
                    case 11:
                        // 音量信息
                        if (data) {
                            if (*(int64_t *)data > 0 && volume == 0) {
                                mpvCoreEvent.fire(VIDEO_UNMUTE);
                            } else if (*(int64_t *)data == 0 && volume > 0) {
                                mpvCoreEvent.fire(VIDEO_MUTE);
                            }
                            volume = *(int64_t *)data;
                            mpvCoreEvent.fire(VIDEO_VOLUME_CHANGE);
                        }
                        break;
                    case 12:
                        if (data) video_paused = *(int *)data;
                        if (video_paused) {
                            brls::Logger::info("========> PAUSE");
                            mpvCoreEvent.fire(MpvEventEnum::MPV_PAUSE);
                        } else if (!video_stopped) {
                            brls::Logger::info("========> RESUME");
                            mpvCoreEvent.fire(MpvEventEnum::MPV_RESUME);
                        }
                        break;
                    case 13:
                        if (data) video_stopped = *(int *)data;

                        break;
                    case 14:
                        if (data) video_seeking = *(int *)data;
                        if (video_seeking) {
                            brls::Logger::info("========> VIDEO SEEKING");
                            mpvCoreEvent.fire(MpvEventEnum::LOADING_START);
                        }
                        break;
                    case 15:
                        if (data) {
                            hwCurrent = *(char **)data;
                            brls::Logger::info("========> HW: {}", hwCurrent);
                            GA("hwdec", {{"hwdec", hwCurrent}})
                        }
                        break;
                    case 16:
                        if (data) filepath = *(char **)data;
                        break;
                    case 17:
                        if (data) video_brightness = *(double *)data;
                        break;
                    case 18:
                        if (data) video_contrast = *(double *)data;
                        break;
                    case 19:
                        if (data) video_saturation = *(double *)data;
                        break;
                    case 20:
                        if (data) video_gamma = *(double *)data;
                        break;
                    case 21:
                        if (data) video_hue = *(double *)data;
                        break;
                    default:
                        break;
                }
                break;
            }
            default:
                break;
        }
    }
}

void MPVCore::reset() {
    brls::Logger::debug("MPVCore::reset");
    mpvCoreEvent.fire(MpvEventEnum::RESET);
    this->percent_pos    = 0;
    this->duration       = 0;  // second
    this->cache_speed    = 0;  // Bps
    this->playback_time  = 0;
    this->video_progress = 0;
    this->mpv_error_code = 0;
    this->video_aspect   = aspectConverter(MPVCore::VIDEO_ASPECT);

    // 软硬解切换后应该手动设置一次渲染尺寸
    // 切换视频前设置渲染尺寸可以顺便将上一条视频的最后一帧画面清空
    setFrameSize(rect);
}

void MPVCore::setUrl(const std::string &url, const std::string &extra, const std::string &method) {
    brls::Logger::debug("{} Url: {}, extra: {}", method, url, extra);

    std::string mpvUrl   = url;
    std::string mpvExtra = extra;

#if defined(PLATFORM_XBOX360) && X360_XAV
    if (MPVCore::HARDWARE_DEC) {
        // Experimental hardware decode: XAV owns the picture, mpv is reduced
        // to audio only.  Only "replace" starts a new XAV stream; backup URLs
        // are appended to mpv's playlist and are not given to XAV.
        if (method == "replace") startXavVideo(url);
        command_async("set", "vid", "no");

        // Bilibili DASH hands us the audio as a separate stream.  Give mpv that
        // URL instead of the video one, otherwise the video stream would be
        // downloaded twice (once by XAV, once by mpv's demuxer).
        std::string audioUrl;
        if (extractAudioOption(extra, audioUrl)) {
            mpvUrl   = audioUrl;
            mpvExtra.clear();
        }
    }
#endif

    if (mpvExtra.empty()) {
        command_async("loadfile", mpvUrl, method);
    } else {
        if (mpvClientApiVersion() >= MPV_MAKE_VERSION(2, 3))
            command_async("loadfile", mpvUrl, method, "0", mpvExtra);
        else
            command_async("loadfile", mpvUrl, method, mpvExtra);
    }
}

void MPVCore::setBackupUrl(const std::string &url, const std::string &extra) { this->setUrl(url, extra, "append"); }

void MPVCore::setVolume(int64_t value) {
    if (value < 0 || value > 100) return;
    command_async("set", "volume", value);
    MPVCore::VIDEO_VOLUME = (int)value;
}

void MPVCore::setVolume(const std::string &value) {
    command_async("set", "volume", value);
    MPVCore::VIDEO_VOLUME = std::stoi(value);
}

int64_t MPVCore::getVolume() const { return this->volume; }

void MPVCore::resume() {
#if defined(PLATFORM_XBOX360) && X360_XAV
    if (xavActive()) brls::Xbox360Xav::instance().setPaused(false);
#endif
    command_async("set", "pause", "no");
}

void MPVCore::pause() {
#if defined(PLATFORM_XBOX360) && X360_XAV
    if (xavActive()) brls::Xbox360Xav::instance().setPaused(true);
#endif
    command_async("set", "pause", "yes");
}

void MPVCore::stop() {
#if defined(PLATFORM_XBOX360) && X360_XAV
    // Drop the XAV player so the next setUrl opens a fresh stream.
    if (MPVCore::HARDWARE_DEC) brls::Xbox360Xav::instance().close();
#endif
    command_async("stop");
}

void MPVCore::seek(int64_t p) {
#if defined(PLATFORM_XBOX360) && X360_XAV
    if (xavActive()) brls::Xbox360Xav::instance().seekAbsolute((double)p);
#endif
    command_async("seek", p, "absolute");
}

void MPVCore::seek(const std::string &p) { command_async("seek", p, "absolute"); }

void MPVCore::seekRelative(int64_t p) {
#if defined(PLATFORM_XBOX360) && X360_XAV
    if (xavActive()) brls::Xbox360Xav::instance().seekAbsolute(this->playback_time + (double)p);
#endif
    command_async("seek", p, "relative");
}

void MPVCore::seekPercent(double p) {
#if defined(PLATFORM_XBOX360) && X360_XAV
    if (xavActive() && this->duration > 0) brls::Xbox360Xav::instance().seekAbsolute(p * (double)this->duration);
#endif
    command_async("seek", p * 100, "absolute-percent");
}

bool MPVCore::isStopped() const { return video_stopped; }

bool MPVCore::isPlaying() const { return video_playing; }

bool MPVCore::isPaused() const { return video_paused; }

double MPVCore::getSpeed() const { return video_speed; }

void MPVCore::setSpeed(double value) {
    if (video_speed != value) command_async("set", "speed", value);
}

void MPVCore::setAspect(const std::string &value) {
    MPVCore::VIDEO_ASPECT = value;
    video_aspect          = aspectConverter(MPVCore::VIDEO_ASPECT);
    if (value == "-2") {
        // 拉伸全屏
        command_async("set", "keepaspect", "no");
        command_async("set", "video-aspect-override", "-1");
        command_async("set", "panscan", "0.0");
    } else if (value == "-3") {
        // 裁剪填充
        command_async("set", "keepaspect", "yes");
        command_async("set", "video-aspect-override", "-1");
        command_async("set", "panscan", "1.0");
    } else {
        // 指定比例
        command_async("set", "keepaspect", "yes");
        command_async("set", "video-aspect-override", MPVCore::VIDEO_ASPECT);
        command_async("set", "panscan", "0.0");
    }
}

void MPVCore::setMirror(bool value) {
    MPVCore::VIDEO_MIRROR = value;
#ifndef BOREALIS_USE_GXM
    command_async("set", "vf", value ? "hflip" : "");
    setHwdecCopyMode(value);
#endif
}

void MPVCore::setHwdecCopyMode(bool value) {
    // 如果正在使用硬解，那么将硬解更新为 auto-copy，避免直接硬解因为不经过 cpu 处理导致镜像翻转、滤镜无效
    if (MPVCore::HARDWARE_DEC) {
        std::string hwdec = value ? "auto-copy" : MPVCore::PLAYER_HWDEC_METHOD;
        command_async("set", "hwdec", hwdec);
        brls::Logger::info("MPV hardware decode: {}", hwdec);
    }
}

void MPVCore::setBrightness(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_BRIGHTNESS = value;
    if (video_brightness != value) command_async("set", "brightness", value);
}

void MPVCore::setContrast(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_CONTRAST = value;
    if (video_contrast != value) command_async("set", "contrast", value);
}

void MPVCore::setSaturation(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_SATURATION = value;
    if (video_saturation != value) command_async("set", "saturation", value);
}

void MPVCore::setGamma(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_GAMMA = value;
    if (video_gamma != value) command_async("set", "gamma", value);
}

void MPVCore::setHue(int value) {
    if (value < -100) value = -100;
    if (value > 100) value = 100;
    MPVCore::VIDEO_HUE = value;
    if (video_hue != value) command_async("set", "hue", value);
}

int MPVCore::getBrightness() const { return video_brightness; }

int MPVCore::getContrast() const { return video_contrast; }

int MPVCore::getSaturation() const { return video_saturation; }

int MPVCore::getGamma() const { return video_gamma; }

int MPVCore::getHue() const { return video_hue; }

// todo: remove these sync function
std::string MPVCore::getString(const std::string &key) {
    if (!mpv) return "";
    char *value = nullptr;
    if (mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_STRING, &value) < 0 || !value) return "";
    std::string result = std::string{value};
    mpvFree(value);
    return result;
}

double MPVCore::getDouble(const std::string &key) {
    if (!mpv) return 0;
    double value = 0;
    if (mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_DOUBLE, &value) < 0) return 0;
    return value;
}

int64_t MPVCore::getInt(const std::string &key) {
    if (!mpv) return 0;
    int64_t value = 0;
    if (mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_INT64, &value) < 0) return 0;
    return value;
}

std::unordered_map<std::string, mpv_node> MPVCore::getNodeMap(const std::string &key) {
    if (!mpv) return {};
    mpv_node node{};
    std::unordered_map<std::string, mpv_node> nodeMap;
    if (mpvGetProperty(mpv, key.c_str(), MPV_FORMAT_NODE, &node) < 0) return nodeMap;
    if (node.format != MPV_FORMAT_NODE_MAP) return nodeMap;
    // todo: 目前不要使用 mpv_node中有指针的部分，因为这些内容指向的内存会在这个函数结束的时候删除
    for (int i = 0; i < node.u.list->num; i++) {
        char *nodeKey = node.u.list->keys[i];
        if (nodeKey == nullptr) continue;
        nodeMap.insert(std::make_pair(std::string{nodeKey}, node.u.list->values[i]));
    }
    mpvFreeNodeContents(&node);
    return nodeMap;
}

double MPVCore::getPlaybackTime() const { return playback_time; }

void MPVCore::disableDimming(bool disable) {
    brls::Logger::info("disableDimming: {}", disable);
    brls::Application::getPlatform()->disableScreenDimming(disable, "Playing video", APPVersion::getPackageName());
    static bool deactivationAvailable = ProgramConfig::instance().getSettingItem(SettingItem::DEACTIVATED_TIME, 0) > 0;
    if (deactivationAvailable) {
        brls::Application::setAutomaticDeactivation(!disable);
    }
}

void MPVCore::setShader(const std::string &profile, const std::string &shaders,
                        const std::vector<std::vector<std::string>> &settings, bool reset) {
    brls::Logger::info("Set shader [{}]: {}", profile, shaders);

    // 如果之前设置的shader包含mpv配置，就需要重置一下
    if (!currentSetting.empty() && reset) clearShader(false);

    currentShaderProfile = profile;
    currentShader        = shaders;
    currentSetting       = settings;

    // 设置着色器
    if (!shaders.empty()) command_async("no-osd", "change-list", "glsl-shaders", "set", shaders);

    // 设置mpv配置
    for (auto &setting : settings) {
        _command_async(setting);
    }

    // 显示通知
    if (reset) brls::Application::notify(profile);
}

void MPVCore::clearShader(bool showHint) {
    brls::Logger::info("Clear shader");

    // 如果当前不涉及mpv配置修改，就无需重置
    bool reset = !currentSetting.empty();

    currentShader.clear();
    currentShaderProfile.clear();
    currentSetting.clear();

    // 清空着色器
    command_async("no-osd", "change-list", "glsl-shaders", "clr", "");

    // 重置mpv配置
    if (reset) MPVCore::instance().restart();

    // 显示通知
    if (showHint) brls::Application::notify("Clear profile");
}

void MPVCore::showOsdText(const std::string &value, int d) { command_async("show-text", value, d); }

void MPVCore::_command_async(const std::vector<std::string> &commands) {
    std::vector<const char *> res;
    res.reserve(commands.size() + 1);
    for (auto &i : commands) {
        res.emplace_back(i.c_str());
    }
    res.emplace_back(nullptr);
#if defined(PLATFORM_XBOX360)
    // P0 §5.5-3 测量：mpv_command_async 理论上不阻塞，但 mpv 内部在
    // set_url/重入时会持锁；若这里出现长耗时即为"冻结主线程"的直接来源。
    const std::uint64_t t_cmd = brls::xbox360perf::nowUsec();
    mpvCommandAsync(mpv, 0, res.data());
    const double cmdMs = brls::xbox360perf::msSince(t_cmd);
    if (cmdMs >= 100.0)
        X360_PROBE_INFO("[x360-mpv] command_async '{}' took {:.0f} ms", commands.empty() ? "?" : commands[0],
                           cmdMs);
#else
    mpvCommandAsync(mpv, 0, res.data());
#endif
}
