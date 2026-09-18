// Examples: combined_demo - demonstrates SimpleMedia's two halves side by
// side: playback of a remote stream plus broadcasting synthetic RGBA frames.
//
//   ./combined_demo [media] [seconds]

#include "SimpleMedia/SimpleMedia.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace
{
    constexpr const char *kSampleUrl = "https://gstreamer.freedesktop.org/data/media/sintel_trailer-480p.webm";

    // Gray test frames look more sensible with a small luminance change per
    // frame so the motion is visible on the receiving side.
    constexpr int kStreamWidth = 1280;
    constexpr int kStreamHeight = 720;

    void onVideoFrameReceived(const SimpleMedia::VideoFrame &frame)
    {
        static int frameCounter = 0;
        if (++frameCounter % 60 == 0)
        {
            std::printf("[Playback] Frame #%d  %dx%d\n", frameCounter, frame.width, frame.height);
        }
    }
} // namespace

int main(int argc, char **argv)
{
    const std::string source = argc > 1 ? argv[1] : kSampleUrl;
    const int seconds = argc > 2 ? std::atoi(argv[2]) : 30;

    // 1) Playback half: ingest a remote stream or local files effortlessly.
    SimpleMedia::VideoPlayer player;
    player.setFrameCallback(onVideoFrameReceived);

    std::printf("Loading playback source: %s\n", source.c_str());
    if (!player.load(source))
    {
        std::fprintf(stderr, "Failed to load playback source.\n");
        return 1;
    }
    player.play();

    // 2) Streaming half: broadcast synthetic frames over RTP.
    SimpleMedia::VideoStreamer streamer;
    if (streamer.setupStream("127.0.0.1", 5000, 5002, kStreamWidth, kStreamHeight))
    {
        streamer.start();

        std::vector<uint8_t> testFrame(static_cast<size_t>(kStreamWidth * kStreamHeight * 4));
        auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
        for (uint32_t frameIndex = 0; std::chrono::steady_clock::now() < end; ++frameIndex)
        {
            // Flat RGBA image with a moving intensity ramp.
            const uint8_t shade = static_cast<uint8_t>((frameIndex * 7) & 0xFF);
            std::fill(testFrame.begin(), testFrame.end(), shade);

            streamer.pushFrame(testFrame.data());

            // Pace to ~30 FPS so the encoder and network stay happy.
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }

        streamer.stop();
    }

    player.stop();
    std::printf("Demo finished.\n");
    return 0;
}