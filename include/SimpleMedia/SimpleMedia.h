#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// SimpleMedia - a thin, header-driven wrapper around GStreamer that hides the
// GStreamer API from client code. It provides a playback pipeline and a
// network streaming pipeline that exchange raw video/audio through callbacks.
//
// Threading notes:
//   * Callbacks run on internal GStreamer streaming threads. The data they
//     receive is only valid for the duration of the callback, so copy it out
//     (e.g. into a SharedFrameBuffer) if you need it later.
//   * setFrameCallback / setAudioCallback may be called from any thread; the
//     library protects the callbacks internally with a mutex.
//   * Audio is delivered as raw PCM via setAudioCallback; no output device is
//     opened by VideoPlayer itself. Forward the decoded blocks to an AudioOutput
//     to actually hear them.
namespace SimpleMedia
{
    // Raw decoded video frame handed to the frame callback.
    struct VideoFrame
    {
        const uint8_t *pixels = nullptr; // RGB (or RGBA, see player caps), tightly packed
        int width = 0;
        int height = 0;
    };

    // Raw decoded audio block handed to the audio callback.
    struct AudioFrame
    {
        const uint8_t *pcmData = nullptr; // Interleaved PCM bytes
        size_t dataSize = 0;              // Size of the buffer in bytes
        int sampleRate = 0;               // e.g. 44100 or 48000 Hz
        int channels = 0;                 // e.g. 1 (Mono) or 2 (Stereo)
    };

    // A thread-safe staging area for the latest decoded video frame.
    // Use it to hand frames from a GStreamer worker thread to your UI thread.
    struct SharedFrameBuffer
    {
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
        bool isNewFrame = false;
        std::mutex mtx;

        // Copies a decoded frame into the shared buffer, re-allocating only
        // when the dimensions change (so steady-state playback reuses memory).
        void update(const uint8_t *rawData, int w, int h)
        {
            std::lock_guard<std::mutex> lock(mtx);
            const size_t targetSize = static_cast<size_t>(w) * static_cast<size_t>(h) * 3;

            if (pixels.size() != targetSize)
            {
                pixels.resize(targetSize);
            }

            if (!pixels.empty() && rawData != nullptr)
            {
                std::memcpy(pixels.data(), rawData, targetSize);
            }

            width = w;
            height = h;
            isNewFrame = true;
        }
    };

    // Media playback pipeline (local file, HTTP/RTSP URL or raw pipeline string)
    // built on top of playbin and appsinks.
    class VideoPlayer
    {
    public:
        struct Impl;

        VideoPlayer();
        ~VideoPlayer();

        // Non-copyable: the implementation owns GStreamer objects.
        VideoPlayer(const VideoPlayer &) = delete;
        VideoPlayer &operator=(const VideoPlayer &) = delete;
        VideoPlayer(VideoPlayer &&) noexcept;
        VideoPlayer &operator=(VideoPlayer &&) noexcept;

        // Called every time a new decoded video frame becomes available.
        // The frame pointer is only valid for the duration of the call.
        void setFrameCallback(std::function<void(const VideoFrame &)> callback);

        // Called every time a new decoded audio block becomes available.
        void setAudioCallback(std::function<void(const AudioFrame &)> callback);

        // Accepts a local file path, an HTTP/RTSP URL, or a raw gst-launch
        // style pipeline description (e.g. an rtpbin + udpsrc receiver).
        // Returns false if the pipeline could not be constructed.
        bool load(const std::string &source);

        void play();
        void pause();
        void stop();

        // Volume in the range 0.0 (mute) .. 1.0 (max).
        void setVolume(double volume);

    private:
        std::unique_ptr<Impl> pImpl; // Hides GStreamer headers from the user
    };

    // Routes decoded PCM audio to the system's default audio output device.
    // Forward the AudioFrames delivered by VideoPlayer::setAudioCallback here
    // so the sound actually reaches the speakers.
    class AudioOutput
    {
    public:
        struct Impl;

        AudioOutput();
        ~AudioOutput();

        // Non-copyable: the implementation owns GStreamer objects.
        AudioOutput(const AudioOutput &) = delete;
        AudioOutput &operator=(const AudioOutput &) = delete;
        AudioOutput(AudioOutput &&) noexcept;
        AudioOutput &operator=(AudioOutput &&) noexcept;

        // Opens the default audio output device. Returns false when no device
        // is available; write() then becomes a no-op.
        bool open();

        // Plays one decoded audio block (interleaved S16LE PCM).
        void write(const AudioFrame &frame);

        // Volume in the range 0.0 (mute) .. 1.0 (max).
        void setVolume(double volume);

        // Stops the device and releases the audio pipeline.
        void stop();

    private:
        std::unique_ptr<Impl> pImpl;
    };

    // Network streaming pipeline: pushes raw RGBA video and interleaved
    // S16LE PCM audio over RTP (H.264 + Opus) to a destination host.
    class VideoStreamer
    {
    public:
        struct Impl;

        VideoStreamer();
        ~VideoStreamer();

        // Non-copyable: the implementation owns GStreamer objects.
        VideoStreamer(const VideoStreamer &) = delete;
        VideoStreamer &operator=(const VideoStreamer &) = delete;
        VideoStreamer(VideoStreamer &&) noexcept;
        VideoStreamer &operator=(VideoStreamer &&) noexcept;

        // Builds an H.264 (video) + Opus (audio) RTP pipeline. Video frames
        // must be RGBA at width x height; audio must be interleaved S16LE
        // stereo at 44100 Hz. Returns false if pipeline assembly failed.
        bool setupStream(const std::string &destinationIp, int videoPort, int audioPort,
                         int width = 1280, int height = 720);

        void start();
        void stop();

        // Feed raw RGBA video frames; they are encoded as H.264 at 30 FPS.
        void pushFrame(const uint8_t *rgbaData);

        // Feed raw interleaved S16LE stereo (44.1 kHz) PCM audio bytes.
        void pushAudio(const uint8_t *pcmData, size_t dataSize);

    private:
        std::unique_ptr<Impl> pImpl;
    };
} // namespace SimpleMedia