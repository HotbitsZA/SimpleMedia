// Examples: stream_app - reads a local media file and broadcasts it over the
// network as H.264 (video) + Opus (audio) RTP using SimpleMedia.
//
//   ./stream_app [file] [destination-ip] [video-port] [audio-port]
//
//   file     - path to a local media file (default: examples/media/Big_Buck_Bunny...)
//   dest-ip  - receiver host (default: 127.0.0.1)
//   video-port / audio-port - UDP ports on the receiver (default: 5000 / 5002)
//
// Pair this with gui_player_app running on the destination machine.

#include "SimpleMedia/SimpleMedia.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace
{
    constexpr int kWidth = 1280;
    constexpr int kHeight = 720;
    constexpr const char *kSampleClipPath = "examples/media/Big_Buck_Bunny_1080_10s_30MB.mp4";

    // Turns a path (relative or absolute) or URL into a playbin-friendly URI.
    std::string toMediaUri(const std::string &source)
    {
        if (source.find("://") != std::string::npos)
            return source;
        if (!source.empty() && source.front() == '/')
            return "file://" + source;

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(source, ec);
        if (ec || source.empty())
            return "file://" + source;
        return "file://" + absolute.lexically_normal().string();
    }

    // Forwards decoded audio blocks straight into the streamer.
    GstFlowReturn on_audio_sample(GstElement *sink, gpointer data)
    {
        auto *streamer = static_cast<SimpleMedia::VideoStreamer *>(data);
        GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample)
            return GST_FLOW_OK;

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ))
        {
            streamer->pushAudio(map.data, map.size);
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // Forwards decoded video frames straight into the streamer.
    GstFlowReturn on_video_sample(GstElement *sink, gpointer data)
    {
        auto *streamer = static_cast<SimpleMedia::VideoStreamer *>(data);
        GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample)
            return GST_FLOW_OK;

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstMapInfo map;
        if (buffer && gst_buffer_map(buffer, &map, GST_MAP_READ))
        {
            streamer->pushFrame(map.data);
            gst_buffer_unmap(buffer, &map);
        }
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // A plain appsink directly usable as a playbin sink. playbin's internal
    // playsink inserts the converters it needs, so enforcing the streamer's
    // exact RGBA frame size is enough here (bins with ghost pads do NOT link).
    GstElement *makeVideoSink(const char *name, SimpleMedia::VideoStreamer *streamer)
    {
        GstElement *sink = gst_element_factory_make("appsink", name);
        if (!sink)
            return nullptr;

        GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                            "format", G_TYPE_STRING, "RGBA",
                                            "width", G_TYPE_INT, kWidth,
                                            "height", G_TYPE_INT, kHeight, nullptr);
        g_object_set(sink, "caps", caps, "emit-signals", TRUE, "sync", TRUE, nullptr);
        gst_caps_unref(caps);
        g_signal_connect(sink, "new-sample", G_CALLBACK(on_video_sample), streamer);
        return sink;
    }

    GstElement *makeAudioSink(const char *name, SimpleMedia::VideoStreamer *streamer)
    {
        GstElement *sink = gst_element_factory_make("appsink", name);
        if (!sink)
            return nullptr;

        GstCaps *caps = gst_caps_new_simple("audio/x-raw",
                                            "format", G_TYPE_STRING, "S16LE",
                                            "layout", G_TYPE_STRING, "interleaved",
                                            "rate", G_TYPE_INT, 44100,
                                            "channels", G_TYPE_INT, 2, nullptr);
        g_object_set(sink, "caps", caps, "emit-signals", TRUE, "sync", TRUE, nullptr);
        gst_caps_unref(caps);
        g_signal_connect(sink, "new-sample", G_CALLBACK(on_audio_sample), streamer);
        return sink;
    }
} // namespace

int main(int argc, char **argv)
{
    const std::string filePath = argc > 1 ? argv[1] : kSampleClipPath;
    const std::string destinationIp = argc > 2 ? argv[2] : "127.0.0.1";
    const int videoPort = argc > 3 ? std::atoi(argv[3]) : 5000;
    const int audioPort = argc > 4 ? std::atoi(argv[4]) : 5002;

    // Bypass macOS Cocoa main-thread checks for a headless GStreamer process.
    g_setenv("GST_MACOS_MAIN_DISABLE", "1", TRUE);
    gst_init(nullptr, nullptr);

    SimpleMedia::VideoStreamer streamer;
    std::printf("[Streamer] Setting up H.264/Opus RTP broadcast to %s:%d/%d...\n",
                destinationIp.c_str(), videoPort, audioPort);
    if (!streamer.setupStream(destinationIp, videoPort, audioPort, kWidth, kHeight))
    {
        std::fprintf(stderr, "Streamer failed setup.\n");
        return 1;
    }
    streamer.start();

    // Reading half: a playbin (decodes any file) whose sinks are our appsinks.
    GstElement *filePipeline = gst_element_factory_make("playbin", "file_reader");
    if (!filePipeline)
    {
        std::fprintf(stderr, "Failed to create the file reader pipeline.\n");
        return 1;
    }
    gst_util_set_object_arg(G_OBJECT(filePipeline), "flags", "video+audio");

    GstElement *videoSink = makeVideoSink("v_file_sink", &streamer);
    GstElement *audioSink = makeAudioSink("a_file_sink", &streamer);
    if (!videoSink || !audioSink)
    {
        std::fprintf(stderr, "Failed to build the reader sink elements.\n");
        return 1;
    }

    g_object_set(filePipeline,
                 "uri", toMediaUri(filePath).c_str(),
                 "video-sink", videoSink,
                 "audio-sink", audioSink,
                 nullptr);

    std::printf("[Streamer] Reading %s and broadcasting...\n", filePath.c_str());
    GstStateChangeReturn ret = gst_element_set_state(filePipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE)
    {
        std::fprintf(stderr, "Failed to start the file reader pipeline.\n");
        return 1;
    }

    // Block until the file finishes (EOS) or an error occurs.
    GstBus *bus = gst_element_get_bus(filePipeline);
    GstMessage *msg = gst_bus_timed_pop_filtered(
        bus, GST_CLOCK_TIME_NONE,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

    if (msg)
    {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
        {
            GError *err = nullptr;
            gchar *debug = nullptr;
            gst_message_parse_error(msg, &err, &debug);
            std::fprintf(stderr, "GStreamer file read error: %s\n", err->message);
            if (debug)
                std::fprintf(stderr, "  (debug info: %s)\n", debug);
            g_error_free(err);
            g_free(debug);
        }
        else if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS)
        {
            std::printf("[Streamer] File finished. Shutting down...\n");
        }
        gst_message_unref(msg);
    }
    else
    {
        // Bus popped with nothing (shouldn't happen with GST_CLOCK_TIME_NONE);
        // drain any messages that arrived in the meantime and shut down.
        GstMessage *drained = gst_bus_timed_pop_filtered(bus, 0, GST_MESSAGE_ANY);
        if (drained)
            gst_message_unref(drained);
    }

    gst_object_unref(bus);
    gst_element_set_state(filePipeline, GST_STATE_NULL);
    gst_object_unref(filePipeline);

    streamer.stop();
    return 0;
}