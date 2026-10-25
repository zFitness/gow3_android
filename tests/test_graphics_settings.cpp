// gport.ini graphics keys (spartanport_settings.cpp, spartanport_graphics.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include "spartanport_graphics.h"
#include "spartanport_settings.h"

namespace G = Gow3Graphics;

static std::string Read(const char* path) {
    std::ifstream in(path);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

static void Env(const char* key, const char* value) {
#ifdef _WIN32
    _putenv_s(key, value);
#else
    if (value[0]) setenv(key, value, 1);
    else unsetenv(key);
#endif
}

int main() {
    assert(G::ParseDisplayMode("borderless") == G::Borderless && G::ParseDisplayMode("bogus") == G::Windowed);
    assert(G::ParseFpsLimit(240) == 240 && G::ParseFpsLimit(75) == 0);
    assert(G::Resolutions[G::ParseResolution("1440p")] == "1440p" && G::ParseResolution("8K") == 0);
    assert(G::ParseEngineFps(240) == 240 && G::ParseEngineFps(90) == 120);
    assert(G::ParseFramesQueued(2) == 2 && G::ParseFramesQueued(0) == 0 && G::ParseFramesQueued(5) == 1);
    assert(G::ParseRcasStrength(100) == 100 && G::ParseRcasStrength(101) == 75 && G::ParseRcasStrength(-1) == 75);
    assert(G::RcasAttenuation(100) == 0.0f && G::RcasAttenuation(0) == 2.0f && G::RcasAttenuation(75) == 0.5f);
    // A window larger than the display loses its right and bottom: 1920x1080 on Android's 720p
    // X screen was cut off instead of shown whole.
    assert((G::ResolveWindowSize({1920, 1080}, {1280, 720}) == G::Size{1280, 720}));
    assert((G::ResolveWindowSize({1920, 1080}, {1920, 1080}) == G::Size{1920, 1080}));
    assert((G::ResolveWindowSize({1920, 1080}, {2560, 1440}) == G::Size{1920, 1080}));  // fits: unchanged
    assert((G::ResolveWindowSize({1920, 1080}, {0, 0}) == G::Size{1920, 1080}));  // no display
    assert((G::ResolveWindowSize({1920, 1080}, {1280, -720}) == G::Size{1920, 1080}));  // invalid
    assert((G::ResolveWindowSize({0, 0}, {1280, 720}) == G::Size{1280, 720}));  // no request
    assert((G::ResolveWindowSize({0, 0}, {0, 0}) == G::DefaultWindowSize));  // neither: 1920x1080
    assert(G::FsrPassRuns(true, false, 1280, 720, 2560, 1440) && !G::FsrPassRuns(false, false, 1280, 720, 2560, 1440));
    assert(G::FsrPassRuns(false, true, 1920, 1080, 1920, 1080) && !G::FsrPassRuns(true, false, 1920, 1080, 1920, 1080));
    assert(!G::FsrPassRuns(true, true, 2560, 1440, 1920, 1080));  // larger image: regular scaling
    assert(G::EffectiveFps(0, 120) == 120 && G::EffectiveFps(240, 120) == 120 && G::EffectiveFps(60, 240) == 60);

    const char* path = "graphics-settings-test.ini";
    {
        std::ofstream out(path);
        out << "display_mode=borderless\nvsync=0\nfps_limit=240\nasync_shaders=0\n"
               "render_resolution=1440p\nengine_fps=240\ndeferred_readback=0\nstale_readback=1\n"
               "fsr1=1\nrcas=0\nrcas_strength=40\nframes_queued=2\n";
        out << "vibration=50\n";
    }
    Env("SPARTAN_CONFIG", path);
    Env("SPARTAN_ASYNC_SHADERS", "");
    auto& v = Gow3Settings::Get();
    Gow3Settings::Load();
    assert(v.display_mode == G::Borderless && !v.vsync && v.fps_limit == 240 && !v.async_shaders);
    assert(v.render_resolution == G::ParseResolution("1440p") && v.engine_fps == 240 && !v.deferred_readback && v.stale_readback);
    assert(v.fsr1 && !v.rcas && v.rcas_strength == 40 && v.frames_queued == 2);
    assert(v.vibration == 50);
    assert(!Gow3Settings::GraphicsNeedRestart());
    v.engine_fps = 120;
    assert(Gow3Settings::GraphicsNeedRestart());
    v.fps_limit = 60;  // live: no restart because of it
    v.engine_fps = 240;
    assert(!Gow3Settings::GraphicsNeedRestart());

    Gow3Settings::Save();
    const std::string saved = Read(path);
    for (const char* line : {"display_mode=borderless\n", "vsync=0\n", "fps_limit=60\n", "async_shaders=0\n",
                             "render_resolution=1440p\n", "engine_fps=240\n", "deferred_readback=0\n",
                             "stale_readback=1\n", "fsr1=1\n", "rcas=0\n", "rcas_strength=40\n",
                             "frames_queued=2\n"}) {
        assert(saved.find(line) != std::string::npos);
    }

    {
        std::ofstream out(path);
        out << "display_mode=huge\nfps_limit=7\nrender_resolution=8K\nengine_fps=1\n"
               "rcas_strength=250\nframes_queued=9\nvibration=250\n";
    }
    Env("SPARTAN_ASYNC_SHADERS", "1");  // 启动时环境变量仍优先生效。
    v.async_shaders = false;
    Gow3Settings::Load();
    assert(v.display_mode == G::Windowed && v.fps_limit == 0 && v.render_resolution == 0 && v.engine_fps == 120);
    assert(v.rcas_strength == 75 && v.frames_queued == 1);
    assert(v.vibration == 100);
    assert(v.async_shaders);
    {
        std::ofstream out(path);
        out << "cheat_double_jumps=1\ncheat_very_hard=1\n";
    }
    Gow3Settings::Load();
    Gow3Settings::Save();
    const std::string cheats = Read(path);
    assert(cheats.find("cheat_double_jumps=1\n") != std::string::npos);
    assert(cheats.find("cheat_very_hard=1\n") != std::string::npos);

    // 包含新增分辨率和曾被错误截断的 4K，保存、重载后档位不能变化。
    for (const auto resolution : G::Resolutions) {
        v.render_resolution = G::ParseResolution(resolution);
        Gow3Settings::Save();
        assert(Read(path).find("render_resolution=" + std::string(resolution) + "\n") != std::string::npos);
        v.render_resolution = -1;
        Gow3Settings::Load();
        assert(G::Resolutions[v.render_resolution] == resolution);
    }

    // 模拟启动器在游戏加载设置后写入长补丁列表；保存必须读取最新磁盘内容。
    const std::string selection = "patches_CUSA01740_01_00=Skip Intro;" + std::string(600, 'x') + ";Disable Bloom\n";
    const std::string extra = selection + "patches_CUSA01715_01_02=\nlsfg_enabled=1\npad.cross=97\n";
    {
        std::ofstream out(path, std::ios::app);
        out << extra;
    }
    v.fps_limit = 30;
    Gow3Settings::Save();
    const std::string preserved = Read(path);
    assert(preserved.find(extra) != std::string::npos);
    assert(preserved.find("fps_limit=30\n") != std::string::npos);
    assert(preserved.find("fps_limit=60\n") == std::string::npos);
    Gow3Settings::Save();
    assert(Read(path) == preserved);
    {
        std::ofstream out(path);
        out << "patches_CUSA01740_01_00=\nlsfg_enabled=0\n";
    }
    Gow3Settings::Save();
    assert(Read(path).find("patches_CUSA01740_01_00=\nlsfg_enabled=0\n") != std::string::npos);
    assert(Read(path).find(selection) == std::string::npos);
    std::remove(path);
    std::puts("PASS: graphics settings");
}
