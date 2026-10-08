/*
 * VLC for PS5: the app.
 *
 * One loop on the main thread, paced by the display (FIFO): read the pad,
 * build the interface, upload VLC's newest picture, draw the video, draw the
 * interface over it, present. VLC decodes on its own threads, sound plays on
 * the audio thread, thumbnails come from the library's worker.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include <stdio.h>
#include <stdlib.h>

#include "backends/imgui_impl_vulkan.h"
#include "gfx.h"
#include "imgui.h"
#include "library.h"
#include "platform.h"
#include "player.h"
#include "ui.h"
#include "network.h"
#include "web.h"

static bool imgui_init()
{
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2((float)gfx.width, (float)gfx.height);
    io.MouseDrawCursor = false;
    ImGui_ImplVulkan_InitInfo init = {};
    init.ApiVersion = VK_API_VERSION_1_1;
    init.Instance = gfx.instance;
    init.PhysicalDevice = gfx.gpu;
    init.Device = gfx.device;
    init.QueueFamily = gfx.queue_family;
    init.Queue = gfx.queue;
    init.DescriptorPoolSize = 2048; /* the font atlas and every thumbnail */
    init.MinImageCount = 2;
    init.ImageCount = gfx.image_count < 2 ? 2 : gfx.image_count;
    init.PipelineInfoMain.RenderPass = gfx.render_pass;
    init.PipelineInfoMain.Subpass = 0;
    init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    return ImGui_ImplVulkan_Init(&init);
}

int main(void)
{
    plat_init();
    /* "start:" lines: if a run stops, the last one says how far it got. */
    fprintf(stderr, "start: VLC starting\n");
    if (!gfx_init()) {
        plat_notify("VLC: no display (see vlc-ps5.log)");
        return 1;
    }
    fprintf(stderr, "start: display ready\n");
    if (!imgui_init()) {
        plat_notify("VLC: interface failed (see vlc-ps5.log)");
        return 1;
    }
    fprintf(stderr, "start: interface renderer ready\n");
    if (!player_init()) {
        plat_notify("VLC: VLC failed to start (see vlc-ps5.log)");
        return 1;
    }
    fprintf(stderr, "start: player ready\n");
    library_init();
    fprintf(stderr, "start: library scanned\n");
    ui_init();
    fprintf(stderr, "start: interface ready, first frame next\n");

    double last = plat_time(), stats_at = last + 10;
    int frames = 0;
    float worst = 0;
    for (;;) {
        double t = plat_time();
        float dt = (float)(t - last);
        last = t;
        PadState pad;
        plat_pad_read(&pad);
        library_update();
        double t_lib = plat_time();

        ImGui_ImplVulkan_NewFrame();
        ImGuiIO &io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)gfx.width, (float)gfx.height);
        io.DeltaTime = dt > 0 ? dt : 1 / 60.0f;
        ImGui::NewFrame();
        ui_frame(pad, dt);
        ImGui::Render();
        double t_ui = plat_time();

        VkCommandBuffer cmd = gfx_begin_frame();
        if (!cmd)
            continue;
        double t_begin = plat_time();
        video_upload(cmd);
        gfx_begin_pass(cmd);
        float vx, vy, vw, vh;
        if (ui_video_rect(&vx, &vy, &vw, &vh))
            video_draw(cmd, vx, vy, vw, vh);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
        gfx_end_frame(cmd);
        double t_end = plat_time();

        if (gfx.frame_number == 1)
            fprintf(stderr, "start: first frame on screen\n");
        if (gfx.frame_number == 3)
            plat_hide_splash();
        /* Frame rate in the log every 10 s: the average and the slowest frame. */
        frames++;
        if (dt > worst)
            worst = dt;
        static double debug_at;
        /* With debug=1 in settings.txt: the player's state in the log while
         * something plays (every 5 s; 1 s with VLCPS5_DEBUG), and the frame
         * rate every 10 s. Off by default: a quiet log for normal use. */
        bool debug = player_debug_log() || getenv("VLCPS5_DEBUG");
        if (debug && t >= debug_at && player_active()) {
            player_debug_line();
            debug_at = t + (getenv("VLCPS5_DEBUG") ? 1 : 5);
        }
        if (t >= stats_at && !debug) {
            frames = 0;
            worst = 0;
            stats_at = t + 10;
        } else if (t >= stats_at) {
            fprintf(stderr, "VLC-PS5: %.1f fps, slowest frame %.1f ms\n", frames / 10.0,
                    worst * 1000);
            frames = 0;
            worst = 0;
            stats_at = t + 10;
        }
        /* A frame that took long: which step (input and library, interface,
         * waiting for a free frame, video upload and present, logging). */
        double t_done = plat_time();
        static double slow_logged;
        if (t_done - t > 0.05 && t_done > slow_logged + 2 && gfx.frame_number > 10) {
            slow_logged = t_done;
            fprintf(stderr, "slow frame %.0f ms: input+library %.1f, interface %.1f, wait %.1f, "
                    "draw+present %.1f, stats %.1f\n", (t_done - t) * 1000, (t_lib - t) * 1000,
                    (t_ui - t_lib) * 1000, (t_begin - t_ui) * 1000, (t_end - t_begin) * 1000,
                    (t_done - t_end) * 1000);
        }
        if (ui_quit_requested() || plat_script_done(gfx.frame_number))
            break;
    }
    fprintf(stderr, "VLC for PS5 quitting\n");
    web_stop();
    net_shutdown();
    library_shutdown();
    player_shutdown();
    gfx_shutdown();
    plat_quit();
    return 0;
}
