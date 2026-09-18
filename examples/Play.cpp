// Examples: play_app - plays a local file, HTTP/RTSP URL or raw pipeline
// description through SimpleMedia, showing video in a native window (with
// both audio and video reaching the system devices) and logging telemetry.
//
//   ./play_app [media] [seconds]
//
//   media   - file path, http(s)/rtsp URL, or a gst-launch style pipeline
//             string (defaults to the bundled Big Buck Bunny clip wherever
//             you run it from, or a public test stream as last resort)
//   seconds - how long to run before shutting down (default: 30)

#include "SimpleMedia/SimpleMedia.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

namespace
{
    // Located relative to the repo root or the build/ dir.
    constexpr const char *kSampleClipCandidates[] = {
        "examples/media/Big_Buck_Bunny_1080_10s_30MB.mp4",
        "../examples/media/Big_Buck_Bunny_1080_10s_30MB.mp4",
    };
    constexpr const char *kFallbackUrl = "https://gstreamer.freedesktop.org/data/media/sintel_trailer-480p.webm";

    // Returns the first clip candidate that actually exists, or the fallback
    // URL. Robust to being launched from the repo root or from build/.
    std::string resolveSampleClip()
    {
        for (const char *candidate : kSampleClipCandidates)
        {
            std::FILE *probe = std::fopen(candidate, "rb");
            if (probe)
            {
                std::fclose(probe);
                return candidate;
            }
        }
        return kFallbackUrl;
    }

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

    struct PlayArgs
    {
        std::string source;
        int seconds;
    };

    int runPlayback(void *userData)
    {
        auto *args = static_cast<PlayArgs *>(userData);

        SimpleMedia::AudioOutput audioOut;
        SimpleMedia::VideoOutput videoOut;
        SimpleMedia::VideoPlayer player;
        player.setFrameCallback([&videoOut](const SimpleMedia::VideoFrame &frame)
                                {
            onVideoFrameReceived(frame);
            videoOut.write(frame);
        });
        player.setAudioCallback([&audioOut](const SimpleMedia::AudioFrame &frame)
                                { audioOut.write(frame); });

        std::printf("Loading media source: %s\n", args->source.c_str());
        if (!player.load(args->source))
        {
            std::fprintf(stderr, "Failed to build the playback pipeline.\n");
            return 1;
        }

        audioOut.open();
        videoOut.open();
        player.play();
        player.setVolume(0.8);
        std::printf("Playback started. Running for %d seconds...\n", args->seconds);

        for (int i = 0; i < args->seconds; ++i)
        {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::printf("Shutting down...\n");
        player.stop();
        audioOut.stop();
        videoOut.stop();
        return 0;
    }
} // namespace

int main(int argc, char **argv)
{
    const std::string source = argc > 1 ? argv[1] : resolveSampleClip();
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 30;

    PlayArgs args{source, seconds};
    return SimpleMedia::runMain(runPlayback, &args);
}