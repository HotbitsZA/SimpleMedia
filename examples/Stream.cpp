// Examples: stream_app - takes a media source (local file path or HTTP/RTSP
// URL) and broadcasts it over the network as H.264 (video) + Opus (audio) RTP
// using SimpleMedia. All GStreamer plumbing is handled inside the library.
//
//   ./stream_app [source] [destination-ip] [video-port] [audio-port]
//
//   source   - path to a local media file or an http(s)/rtsp URL
//              (default: examples/media/Big_Buck_Bunny_1080_10s_30MB.mp4)
//   dest-ip  - receiver host (default: 127.0.0.1)
//   video-port / audio-port - UDP ports on the receiver (default: 5000 / 5002)
//
// Pair this with gui_player_app running on the destination machine.

#include "SimpleMedia/SimpleMedia.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace
{
    // Located relative to the repo root or the build/ dir.
    constexpr const char *kSampleClipCandidates[] = {
        "examples/media/Big_Buck_Bunny_1080_10s_30MB.mp4",
        "../examples/media/Big_Buck_Bunny_1080_10s_30MB.mp4",
    };

    // Returns the first clip candidate that actually exists, so stream_app
    // works whether run from the repo root or from build/.
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
        return kSampleClipCandidates[0]; // Let the library report the error.
    }
} // namespace

int main(int argc, char **argv)
{
    const std::string source = argc > 1 ? argv[1] : resolveSampleClip();
    const std::string destinationIp = argc > 2 ? argv[2] : "127.0.0.1";
    const int videoPort = argc > 3 ? std::atoi(argv[3]) : 5000;
    const int audioPort = argc > 4 ? std::atoi(argv[4]) : 5002;

    std::printf("[Streamer] Streaming %s to %s:%d/%d...\n",
                source.c_str(), destinationIp.c_str(), videoPort, audioPort);

    SimpleMedia::VideoStreamer streamer;
    if (!streamer.streamSource(source, destinationIp, videoPort, audioPort))
    {
        std::fprintf(stderr, "[Streamer] Streaming failed (source ended in error).\n");
        return 1;
    }

    std::printf("[Streamer] Finished.\n");
    return 0;
}