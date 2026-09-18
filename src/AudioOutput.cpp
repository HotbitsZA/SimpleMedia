#include "SimpleMedia/SimpleMedia.h"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <cstdio>
#include <cstring>
#include <mutex>

namespace SimpleMedia
{
    struct AudioOutput::Impl
    {
        GstElement *pipeline = nullptr;
        GstElement *appsrc = nullptr;
        GstElement *volume = nullptr;
        int sampleRate = 0;    // Caps of the last block we pushed
        int channels = 0;
        std::mutex mtx;        // Guards pipeline/volume use across threads
    };
} // namespace SimpleMedia

namespace
{
    // The default audio device in `appsrc is-live` mode plays buffers as soon
    // as they arrive (the player appsink already paces them in real time), so
    // no deep queue is needed here.
    constexpr const char *kOutputPipeline =
        "appsrc name=src format=time is-live=true do-timestamp=true "
        "! audioconvert ! audioresample ! volume name=vol ! autoaudiosink";
} // namespace

namespace SimpleMedia
{
    AudioOutput::AudioOutput() : pImpl(std::make_unique<Impl>())
    {
        // Initialise GStreamer exactly once, without touching the command line.
        static std::once_flag initFlag;
        std::call_once(initFlag, []()
                       { gst_init(nullptr, nullptr); });
    }

    AudioOutput::AudioOutput(AudioOutput &&) noexcept = default;
    AudioOutput &AudioOutput::operator=(AudioOutput &&) noexcept = default;

    AudioOutput::~AudioOutput()
    {
        if (pImpl)
        {
            stop();
        }
    }

    bool AudioOutput::open()
    {
        stop();

        GstElement *pipeline = gst_parse_launch(kOutputPipeline, nullptr);
        if (!pipeline)
        {
            std::fprintf(stderr, "[SimpleMedia] AudioOutput: failed to build the "
                                 "audio output pipeline.\n");
            return false;
        }

        GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
        GstElement *volume = gst_bin_get_by_name(GST_BIN(pipeline), "vol");
        if (!appsrc || !volume || gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
                                      GST_STATE_CHANGE_FAILURE)
        {
            if (appsrc)
                gst_object_unref(appsrc);
            if (volume)
                gst_object_unref(volume);
            gst_object_unref(pipeline);
            std::fprintf(stderr, "[SimpleMedia] AudioOutput: no usable audio "
                                 "output device.\n");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(pImpl->mtx);
            pImpl->pipeline = pipeline;
            pImpl->appsrc = appsrc;
            pImpl->volume = volume;
        }
        return true;
    }

    void AudioOutput::write(const AudioFrame &frame)
    {
        if (!frame.pcmData || frame.dataSize == 0)
            return;

        std::lock_guard<std::mutex> lock(pImpl->mtx);
        if (!pImpl->pipeline || !pImpl->appsrc)
            return;

        const int rate = frame.sampleRate > 0 ? frame.sampleRate : 44100;
        const int channels = frame.channels > 0 ? frame.channels : 2;

        if (pImpl->sampleRate != rate || pImpl->channels != channels)
        {
            GstCaps *caps = gst_caps_new_simple("audio/x-raw",
                                                "format", G_TYPE_STRING, "S16LE",
                                                "layout", G_TYPE_STRING, "interleaved",
                                                "rate", G_TYPE_INT, rate,
                                                "channels", G_TYPE_INT, channels, nullptr);
            g_object_set(pImpl->appsrc, "caps", caps, nullptr);
            gst_caps_unref(caps);
            pImpl->sampleRate = rate;
            pImpl->channels = channels;
        }

        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frame.dataSize, nullptr);
        if (!buffer)
            return;

        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_WRITE))
        {
            std::memcpy(map.data, frame.pcmData, frame.dataSize);
            gst_buffer_unmap(buffer, &map);
        }

        const GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(pImpl->appsrc), buffer);
        if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING)
        {
            std::fprintf(stderr, "[SimpleMedia] AudioOutput: push failed "
                                 "(flow=%s).\n", gst_flow_get_name(ret));
        }
    }

    void AudioOutput::setVolume(double volume)
    {
        std::lock_guard<std::mutex> lock(pImpl->mtx);
        if (pImpl->volume)
        {
            g_object_set(pImpl->volume, "volume", volume, nullptr);
        }
    }

    void AudioOutput::stop()
    {
        std::lock_guard<std::mutex> lock(pImpl->mtx);
        if (pImpl->appsrc)
        {
            gst_object_unref(pImpl->appsrc);
            pImpl->appsrc = nullptr;
        }
        if (pImpl->volume)
        {
            gst_object_unref(pImpl->volume);
            pImpl->volume = nullptr;
        }
        if (pImpl->pipeline)
        {
            gst_element_set_state(pImpl->pipeline, GST_STATE_NULL);
            gst_object_unref(pImpl->pipeline);
            pImpl->pipeline = nullptr;
        }
        pImpl->sampleRate = 0;
        pImpl->channels = 0;
    }
} // namespace SimpleMedia