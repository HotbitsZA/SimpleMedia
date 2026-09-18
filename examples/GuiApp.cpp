// Examples: gui_player_app - Dear ImGui + OpenGL video player that renders the
// frames decoded by SimpleMedia. By default it listens for the RTP broadcast
// produced by stream_app (H.264 on UDP 5000, Opus on UDP 5002). Pass a file
// path, URL, or pipeline string as the first argument to play that instead.
//
//   ./gui_player_app [media]

#include "SimpleMedia/SimpleMedia.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h> // Native macOS OpenGL header

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>

namespace
{
    // Thread-safe bridge between GStreamer worker threads and the GL thread.
    struct RenderContext
    {
        SimpleMedia::SharedFrameBuffer frameBuffer;
        std::atomic<bool> isRunning{true};
        GLuint openGlTextureId = 0;
    };

    void glfw_error_callback(int error, const char *description)
    {
        std::cerr << "GLFW Error " << error << ": " << description << std::endl;
    }

    // Reads the latest frame dimensions under the lock so the render thread
    // never races with the GStreamer thread.
    SimpleMedia::VideoFrame snapshot(SimpleMedia::SharedFrameBuffer &fb)
    {
        std::lock_guard<std::mutex> lock(fb.mtx);
        return SimpleMedia::VideoFrame{fb.pixels.data(), fb.width, fb.height};
    }

    constexpr const char *kLivePipeline =
        "rtpbin name=rtpbin "
        "udpsrc port=5000 caps=\"application/x-rtp,media=video,clock-rate=90000,encoding-name=H264\""
        " ! rtpbin.recv_rtp_sink_0 "
        "rtpbin. ! rtph264depay ! decodebin ! videoconvert ! appsink name=videosink "
        "udpsrc port=5002 caps=\"application/x-rtp,media=audio,clock-rate=48000,encoding-name=OPUS\""
        " ! rtpbin.recv_rtp_sink_1 "
        "rtpbin. ! rtpopusdepay ! decodebin ! audioconvert "
        "! appsink name=audiosink caps=\"audio/x-raw, format=S16LE, layout=interleaved\"";
} // namespace

int main(int argc, char **argv)
{
    const std::string source = argc > 1 ? argv[1] : kLivePipeline;

    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit())
    {
        std::cerr << "Failed to initialise GLFW." << std::endl;
        return 1;
    }

    // Modern Core Profile OpenGL 3.3 (required on macOS).
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(1400, 900, "SimpleMedia Framework - Dear ImGui Player",
                                          nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        std::cerr << "Failed to create the GLFW window." << std::endl;
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // VSync

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    RenderContext ctx;
    SimpleMedia::AudioOutput audioOut;
    SimpleMedia::VideoPlayer player;

    // Copy freshly decoded frames into the shared bridge from GStreamer threads.
    player.setFrameCallback([&ctx](const SimpleMedia::VideoFrame &frame)
                            { ctx.frameBuffer.update(frame.pixels, frame.width, frame.height); });
    // Route decoded audio to the system output device.
    player.setAudioCallback([&audioOut](const SimpleMedia::AudioFrame &frame)
                            { audioOut.write(frame); });

    if (!player.load(source))
    {
        std::cerr << "Failed to load media source: " << source << std::endl;
        return 1;
    }
    audioOut.open();
    audioOut.setVolume(0.8);
    player.play();

    glGenTextures(1, &ctx.openGlTextureId);
    glBindTexture(GL_TEXTURE_2D, ctx.openGlTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    while (!glfwWindowShouldClose(window) && ctx.isRunning)
    {
        glfwPollEvents();

        // 1. Upload any newly arrived frame to the GPU (RGB, 3 bytes/pixel).
        {
            std::lock_guard<std::mutex> lock(ctx.frameBuffer.mtx);
            if (ctx.frameBuffer.isNewFrame && !ctx.frameBuffer.pixels.empty())
            {
                glBindTexture(GL_TEXTURE_2D, ctx.openGlTextureId);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                             ctx.frameBuffer.width, ctx.frameBuffer.height,
                             0, GL_RGB, GL_UNSIGNED_BYTE, ctx.frameBuffer.pixels.data());
                ctx.frameBuffer.isNewFrame = false;
            }
        }

        // 2. Begin the ImGui frame.
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        const SimpleMedia::VideoFrame frame = snapshot(ctx.frameBuffer);

        ImGui::Begin("SimpleMedia Video Player");
        ImGui::Text("Resolution Target (%dx%d)", frame.width, frame.height);

        if (ImGui::Button("PLAY"))
            player.play();
        ImGui::SameLine();
        if (ImGui::Button("PAUSE"))
            player.pause();
        ImGui::SameLine();
        if (ImGui::Button("STOP"))
            player.stop();

        static float currentVolume = 0.8f;
        if (ImGui::SliderFloat("Audio Volume", &currentVolume, 0.0f, 1.0f))
        {
            player.setVolume(static_cast<double>(currentVolume));
            audioOut.setVolume(static_cast<double>(currentVolume));
        }

        ImGui::Separator();

        if (ctx.openGlTextureId != 0 && frame.width > 0 && frame.height > 0)
        {
            const ImVec2 displaySize(static_cast<float>(frame.width),
                                     static_cast<float>(frame.height));
            ImGui::Image(reinterpret_cast<void *>(static_cast<intptr_t>(ctx.openGlTextureId)),
                         displaySize);
        }
        else
        {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                               "Buffering stream or loading source asset...");
        }

        ImGui::End();

        // 3. Render pass.
        ImGui::Render();
        int displayW = 0, displayH = 0;
        glfwGetFramebufferSize(window, &displayW, &displayH);
        glViewport(0, 0, displayW, displayH);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    player.stop();
    audioOut.stop();
    glDeleteTextures(1, &ctx.openGlTextureId);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}