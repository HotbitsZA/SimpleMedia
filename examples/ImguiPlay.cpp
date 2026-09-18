// Examples: gui_player_basic_app - a minimal Dear ImGui + OpenGL video player.
// A leaner variant of gui_player_app with just a video texture and no controls.
//
//   ./gui_player_basic_app [media]

#include "SimpleMedia/SimpleMedia.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <OpenGL/gl3.h>

#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>

int main(int argc, char **argv)
{
    const std::string source = argc > 1 ? argv[1]
                                        : "https://gstreamer.freedesktop.org/data/media/sintel_trailer-480p.webm";

    if (!glfwInit())
    {
        std::cerr << "Failed to initialise GLFW." << std::endl;
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(1280, 720, "SimpleMedia - Basic ImGui Player",
                                          nullptr, nullptr);
    if (!window)
    {
        glfwTerminate();
        std::cerr << "Failed to create the GLFW window." << std::endl;
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    // Thread-safe bridge between GStreamer and the render loop.
    SimpleMedia::SharedFrameBuffer videoBridge;
    SimpleMedia::AudioOutput audioOut;
    SimpleMedia::VideoPlayer player;
    player.setFrameCallback([&videoBridge](const SimpleMedia::VideoFrame &frame)
                            { videoBridge.update(frame.pixels, frame.width, frame.height); });
    player.setAudioCallback([&audioOut](const SimpleMedia::AudioFrame &frame)
                            { audioOut.write(frame); });

    if (!player.load(source))
    {
        std::cerr << "Failed to load: " << source << std::endl;
        return 1;
    }
    audioOut.open();
    player.play();

    GLuint videoTextureId = 0;
    glGenTextures(1, &videoTextureId);
    glBindTexture(GL_TEXTURE_2D, videoTextureId);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    while (!glfwWindowShouldClose(window))
    {
        glfwPollEvents();

        // Upload new frames to the GPU on the render thread.
        {
            std::lock_guard<std::mutex> lock(videoBridge.mtx);
            if (videoBridge.isNewFrame && !videoBridge.pixels.empty())
            {
                glBindTexture(GL_TEXTURE_2D, videoTextureId);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB,
                             videoBridge.width, videoBridge.height, 0,
                             GL_RGB, GL_UNSIGNED_BYTE, videoBridge.pixels.data());
                videoBridge.isNewFrame = false;
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::Begin("Video Playback");
        {
            std::lock_guard<std::mutex> lock(videoBridge.mtx);
            if (videoBridge.width > 0 && videoBridge.height > 0)
            {
                const ImTextureID textureHandle =
                    reinterpret_cast<ImTextureID>(static_cast<intptr_t>(videoTextureId));
                ImGui::Image(textureHandle, ImVec2(static_cast<float>(videoBridge.width),
                                                   static_cast<float>(videoBridge.height)));
            }
            else
            {
                ImGui::Text("Waiting for stream payload...");
            }
        }
        ImGui::End();

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
    glDeleteTextures(1, &videoTextureId);

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();
    return 0;
}