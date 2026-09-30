// SPDX-License-Identifier: GPL-2.0-or-later
#include <windows.h>
#include <obs.h>
#include <filesystem>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t **argv)
{
    if (argc != 7) return 64;
    const std::filesystem::path root(argv[1]);
    HANDLE events[5]{};
    for (int i = 0; i != 5; ++i) {
        events[i] = OpenEventW(EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, argv[i + 2]);
        if (!events[i]) {
            for (auto event : events) if (event) CloseHandle(event);
            return 65;
        }
    }
    int result = 1;
    bool started = false;
    do {
        if (obs_get_version() != LIBOBS_API_VER) break;
        auto profile = (root / "profile").string();
        started = obs_startup("en-US", profile.c_str(), nullptr);
        if (!started) break;
        obs_video_info video{};
        auto graphics = (root / "bin/64bit/libobs-d3d11.dll").string();
        video.graphics_module = graphics.c_str();
        video.fps_num = 60;
        video.fps_den = 1;
        video.base_width = video.output_width = 64;
        video.base_height = video.output_height = 64;
        video.output_format = VIDEO_FORMAT_RGBA;
        video.colorspace = VIDEO_CS_709;
        video.range = VIDEO_RANGE_FULL;
        video.scale_type = OBS_SCALE_DISABLE;
        if (obs_reset_video(&video) != OBS_VIDEO_SUCCESS) break;
        obs_module_t *module = nullptr;
        auto plugin = (root / "obs-plugins/64bit/obs-nr-filter.dll").string();
        auto data = (root / "data/obs-plugins/obs-nr-filter").string();
        if (obs_open_module(&module, plugin.c_str(), data.c_str()) != MODULE_SUCCESS || !obs_init_module(module)) break;
        obs_module_t *encoder = nullptr;
        auto encoder_path = (root / "obs-plugins/64bit/obs-x264.dll").string();
        auto encoder_data = (root / "data/obs-plugins/obs-x264").string();
        if (obs_open_module(&encoder, encoder_path.c_str(), encoder_data.c_str()) != MODULE_SUCCESS || !obs_init_module(encoder)) break;
        bool x264 = false;
        const char *id = nullptr;
        for (size_t i = 0; obs_enum_encoder_types(i, &id); ++i) if (std::string(id) == "obs_x264") x264 = true;
        if (!x264) break;
        std::cout << "NR_QA_READY libobs=" << obs_get_version_string() << " module=obs-nr-filter encoder=obs_x264\n" << std::flush;
        SetEvent(events[0]);
        if (WaitForSingleObject(events[1], 15000) != WAIT_OBJECT_0) break;
        auto *settings = obs_data_create();
        obs_data_set_int(settings, "content_id", 0xA17);
        const bool same = obs_data_get_int(settings, "content_id") == 0xA17;
        obs_data_release(settings);
        if (!same) break;
        std::cout << "NR_QA_ACTION content_id=2583\n" << std::flush;
        SetEvent(events[2]);
        if (WaitForSingleObject(events[3], 15000) != WAIT_OBJECT_0) break;
        result = 0;
    } while (false);
    if (started) obs_shutdown();
    std::cout << "NR_QA_EXIT cleanup=complete result=" << result << "\n" << std::flush;
    SetEvent(events[4]);
    for (auto event : events) CloseHandle(event);
    return result;
}
