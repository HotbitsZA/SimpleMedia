# SimpleMedia

A small C++17 media library for macOS/Linux built on [GStreamer](https://gstreamer.freedesktop.org/).
It wraps GStreamer into a simple C++ API for decoding and playing media, and for streaming
live H.264 + Opus over RTP/UDP.

## Features

- `SimpleMedia::VideoPlayer` — decode and play local files, HTTP(S)/RTSP URLs, or raw
  `gst-launch` style pipelines; receive decoded frames/audio via callbacks.
- `SimpleMedia::VideoStreamer` — push RGBA frames and S16LE audio into a live
  H.264 (`x264enc`/`vtenc_h264`) + Opus RTP broadcast to any UDP host.
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

If the source has no audio track (the bundled clip is video-only), the streamer
synthesises silence so the audio branch keeps flowing and the pipeline never stalls.

## API at a glance

```cpp
#include "SimpleMedia/SimpleMedia.h"

// --- decode/play ---
SimpleMedia::VideoPlayer player;
player.setFrameCallback([](const SimpleMedia::VideoFrame &f) { /* RGBA pixels */ });
player.setAudioCallback([](const SimpleMedia::AudioFrame &a) { /* S16LE audio */ });
player.load("clip.mp4");
player.play();

// --- stream ---
SimpleMedia::VideoStreamer streamer;
streamer.setupStream("192.168.1.10", 5000, 5002, 1280, 720);
streamer.start();
streamer.pushFrame(rgbaBuffer);     // 1280x720 RGBA, call at ~30 FPS
streamer.pushAudio(pcm16, nBytes);  // optional; silence is sent otherwise
```

## License

MIT. Dependencies (GStreamer plugins, Dear ImGui, GLFW) are licensed separately.