// Examples: play_app - plays a local file, HTTP/RTSP URL or raw pipeline
// description through SimpleMedia and logs frame/audio telemetry.
//
//   ./play_app [media] [seconds]
//
//   media   - file path, http(s)/rtsp URL, or a gst-launch style pipeline
//             string (defaults to the bundled Big Buck Bunny clip or a
//             public RTSP test stream)
//   seconds - how long to run before shutting down (default: 30)

#include "SimpleMedia/SimpleMedia.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace
{
    constexpr const char *kSampleClipPath = "examples/media/Big_Buck_Bunny_1080_10s_30MB.mp4";
    constexpr const char *kFallbackUrl = "https://gstreamer.freedesktop.org/data/media/sintel_trailer-480p.webm";

    void onVideoFrameReceived(const SimpleMedia::VideoFrame &frame)
    {
        static int frameCounter = 0;
        ++frameCounter;

        if (frameCounter % 60 == 0)
        {
            std::printf("[Video] Frame #%d  %dx%d  %p\n",
                        frameCounter, frame.width, frame.height,
                        static_cast<const void *>(frame.pixels));
        }
    }

    void onAudioFrameReceived(const SimpleMedia::AudioFrame &frame)
    {
        static int audioBlockCounter = 0;
        ++audioBlockCounter;

        if (audioBlockCounter % 100 == 0)
        {
            std::printf("[Audio] Block #%d  %zu bytes  %d Hz  %d ch\n",
                        audioBlockCounter, frame.dataSize, frame.sampleRate, frame.channels);
        }
    }
} // namespace

int main(int argc, char **argv)
{
    std::string source = argc > 1 ? argv[1] : kSampleClipPath;
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 30;

    // Prefer the bundled clip when it exists, otherwise fall back to a URL.
    if (argc == 1)
    {
        std::FILE *probe = std::fopen(kSampleClipPath, "rb");
        if (probe)
        {
            std::fclose(probe);
        }
        else
        {
            source = kFallbackUrl;
        }
    }

    SimpleMedia::AudioOutput audioOut;
    SimpleMedia::VideoPlayer player;
    player.setFrameCallback(onVideoFrameReceived);
    //player.setAudioCallback(onAudioFrameReceived);
    player.setAudioCallback([&audioOut](const SimpleMedia::AudioFrame &frame)
                            { audioOut.write(frame); });

    std::printf("Loading media source: %s\n", source.c_str());
    if (!player.load(source))
    {
        std::fprintf(stderr, "Failed to build the playback pipeline.\n");
        return 1;
    }

    audioOut.open();
    player.play();
    player.setVolume(0.8);
    std::printf("Playback started. Running for %d seconds...\n", seconds);

    for (int i = 0; i < seconds; ++i)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    std::printf("Shutting down...\n");
    player.stop();
    audioOut.stop();
    return 0;
}