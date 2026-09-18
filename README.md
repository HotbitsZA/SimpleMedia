# SimpleMedia

A small C++17 media library for macOS/Linux built on [GStreamer](https://gstreamer.freedesktop.org/).
It wraps GStreamer into a simple C++ API for decoding and playing media, and for streaming
live H.264 + Opus over RTP/UDP.

## Features

- `SimpleMedia::VideoPlayer` — decode and play local files, HTTP(S)/RTSP URLs, or raw
  `gst-launch` style pipelines; receive decoded frames/audio via callbacks.
- `SimpleMedia::VideoOutput` — shows decoded frames in the system's default video
  window so playback can be watched without a custom renderer.
- `SimpleMedia::AudioOutput` — pipes the decoded PCM blocks to the system's default
  audio device so video playback is actually audible.
- `SimpleMedia::VideoStreamer` — two ways to broadcast H.264 (`x264enc`/`vtenc_h264`) + Opus
  RTP to any UDP host: push raw RGBA frames + S16LE audio yourself, or hand it a
  media source (file path / URL) with `streamSource()` and let the library decode
  and stream it end-to-end.
- Zero warnings with `-Wall -Wextra` (AppleClang 21 / GCC 13).
- Optional Dear ImGui + GLFW GUI examples.

## Layout

```
CMakeLists.txt              Build & install rules
include/SimpleMedia/        Public headers
    SimpleMedia.h
    AudioRingBuffer.h
src/                        Library implementation
    VideoPlayer.cpp
    VideoStreamer.cpp
examples/                   Example programs
    Play.cpp        play_app       Console player + frame telemetry
    Stream.cpp      stream_app     Sends a local file as RTP/UDP to a receiver
    main.cpp        combined_demo  Combined demo/test
    GuiApp.cpp      gui_player_app        ImGui player
    ImguiPlay.cpp   gui_player_basic_app  ImGui player (minimal)
    media/          Bundled sample clip (Big Buck Bunny, 10s)
```

## Requirements

- CMake >= 3.16, a C++17 compiler
- GStreamer 1.x core plugins plus:

  `gstreamer-1.0 gstreamer-app-1.0 gstreamer-audio-1.0 gstreamer-video-1.0`,
  `gst-plugins-base`, `gst-plugins-good`, `gst-plugins-bad`, `gst-plugins-ugly`
  (for `x264enc`), and `gst-libav` (optional, for extra decoders).

On macOS (Homebrew):

```sh
brew install gstreamer gst-plugins-base gst-plugins-good gst-plugins-bad gst-plugins-ugly
```

### Optional: GUI examples

The two ImGui examples need GLFW and the Dear ImGui source tree. Point CMake at your
ImGui checkout:

```sh
brew install glfw
# assumes ../../OpenSource/imgui from this repo:
cmake -S . -B build -DSIMPLEMEDIA_IMGUI_DIR="$(cd ../../OpenSource/imgui && pwd)"
```

If ImGui is not available the build continues with the CLI examples only.

## Building

```sh
cmake -S . -B build
cmake --build build -j
```

For the GUI examples, pass `-DSIMPLEMEDIA_IMGUI_DIR=<path>` to the configure step above.
Disable them with `-DSIMPLEMEDIA_BUILD_GUI_EXAMPLES=OFF`.

## Usage

### Play a file

```sh
./build/play_app media/Big_Buck_Bunny_1080_10s_30MB.mp4
```

You can also pass an HTTP(S)/RTSP URL or a raw `gst-launch` pipeline description.

### Stream a file over the network

Start a receiver on another machine (or localhost), e.g. with GStreamer:

```sh
# video on UDP/5000, audio on UDP/5002
gst-launch-1.0 udpsrc port=5000 ! "application/x-rtp,media=video,encoding-name=H264,payload=96" \
    ! rtph264depay ! h264parse ! avdec_h264 ! autovideosink
gst-launch-1.0 udpsrc port=5002 ! "application/x-rtp,media=audio,encoding-name=OPUS,payload=96" \
    ! rtpopusdepay ! opusdec ! autoaudiosink
```

Then broadcast:

```sh
./build/stream_app media/Big_Buck_Bunny_1080_10s_30MB.mp4 <host> 5000 5002
```

The example is a thin wrapper around the library's one-call `streamSource()`:

```cpp
SimpleMedia::VideoStreamer streamer;
streamer.streamSource("clip.mp4", "192.168.1.10", 5000, 5002); // blocks until EOS
```

`streamSource()` builds the encoder pipeline, decodes the source internally and
feeds it into the stream until the source ends or errors. If the source has no
audio track (the bundled clip is video-only), the streamer synthesises silence so
the audio branch keeps flowing and the pipeline never stalls.

## API at a glance

```cpp
#include "SimpleMedia/SimpleMedia.h"

// --- decode/play ---
SimpleMedia::VideoPlayer player;
SimpleMedia::VideoOutput videoOut; // show decoded frames in a native window
SimpleMedia::AudioOutput audioOut; // route decoded PCM to the speakers
player.setFrameCallback([&videoOut](const SimpleMedia::VideoFrame &f) { videoOut.write(f); });
player.setAudioCallback([&audioOut](const SimpleMedia::AudioFrame &a) { audioOut.write(a); });
player.load("clip.mp4");
videoOut.open();
audioOut.open();
player.play();

// --- stream ---
// (a) Stream a media source end-to-end (file path or URL). Blocks until the
//     source finishes or errors.
SimpleMedia::VideoStreamer streamer;
streamer.streamSource("clip.mp4", "192.168.1.10", 5000, 5002);

// (b) Non-blocking: returns immediately; onFinished runs on an internal thread
//     when the stream ends (true on full EOS, false on error/cancellation).
streamer.startStreamingSource("clip.mp4", "192.168.1.10", 5000, 5002,
                              [](bool ok) { std::printf("stream done: %d\n", ok); });
// ... later, to cancel an in-flight stream:
streamer.stop();

// (c) Or feed raw frames yourself (e.g. from a camera):
SimpleMedia::VideoStreamer enc;
enc.setupStream("192.168.1.10", 5000, 5002, 1280, 720);
enc.start();
enc.pushFrame(rgbaBuffer);     // 1280x720 RGBA, call at ~30 FPS
enc.pushAudio(pcm16, nBytes);  // optional; silence is sent otherwise
```

### macOS note

Native GStreamer video windows on macOS require the Cocoa main loop to pump on
the main thread. Apps that use `VideoOutput` (or `playbin` with a non-appsink
video sink) should start their logic via `SimpleMedia::runMain()`:

```cpp
int myApp(void *userData)
{
    // All your video / audio setup and sleep loops go here.
    // This function runs on a worker thread; the main thread runs Cocoa.
    return 0;
}

int main(int argc, char **argv)
{
    return SimpleMedia::runMain(myApp, nullptr);
}
```

On Linux and Windows this simply calls your function directly.

## License

MIT. Dependencies (GStreamer plugins, Dear ImGui, GLFW) are licensed separately.