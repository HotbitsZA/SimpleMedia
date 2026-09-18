#include "SimpleMedia/SimpleMedia.h"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <cstdio>
#include <filesystem>
#include <mutex>

namespace SimpleMedia
{
    struct VideoPlayer::Impl
    {
        GstElement *pipeline = nullptr;
        std::function<void(const VideoFrame &)> frameCallback;
        std::function<void(const AudioFrame &)> audioCallback;
        std::mutex callbackMtx; // Guards the callbacks against concurrent (un)registration
    };
} // namespace SimpleMedia

namespace
{
    // GStreamer logs a warning (and ignores) unknown playbin flag names, so keep
    // this to the flags that actually exist on playbin/playbin3.
    constexpr const char *kPlaybinFlags = "video+audio";

    // Called on the audio streaming thread whenever a decoded audio block
    // leaves the appsink.
    GstFlowReturn on_new_audio_sample(GstElement *sink, gpointer data)
    {
        auto *impl = static_cast<SimpleMedia::VideoPlayer::Impl *>(data);

        SimpleMedia::AudioFrame frame;
        {
            std::lock_guard<std::mutex> lock(impl->callbackMtx);
            if (!impl->audioCallback)
                return GST_FLOW_OK;
        }

        GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample)
            return GST_FLOW_OK;

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstCaps *caps = gst_sample_get_caps(sample);

        if (buffer && caps)
        {
            GstStructure *structure = gst_caps_get_structure(caps, 0);
            gst_structure_get_int(structure, "rate", &frame.sampleRate);
            gst_structure_get_int(structure, "channels", &frame.channels);

            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ))
            {
                frame.pcmData = map.data;
                frame.dataSize = map.size;

                // Grab the callback under the lock again; a user callback may
                // have been swapped in/out in the meantime.
                std::lock_guard<std::mutex> lock(impl->callbackMtx);
                if (impl->audioCallback)
                    impl->audioCallback(frame);

                gst_buffer_unmap(buffer, &map);
            }
        }

        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // Called on the video streaming thread whenever a decoded video frame
    // leaves the appsink.
    GstFlowReturn on_new_sample(GstElement *sink, gpointer data)
    {
        auto *impl = static_cast<SimpleMedia::VideoPlayer::Impl *>(data);

        SimpleMedia::VideoFrame frame;
        {
            std::lock_guard<std::mutex> lock(impl->callbackMtx);
            if (!impl->frameCallback)
                return GST_FLOW_OK;
        }

        GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
        if (!sample)
            return GST_FLOW_OK;

        GstBuffer *buffer = gst_sample_get_buffer(sample);
        GstCaps *caps = gst_sample_get_caps(sample);

        if (buffer && caps)
        {
            GstStructure *structure = gst_caps_get_structure(caps, 0);
            gst_structure_get_int(structure, "width", &frame.width);
            gst_structure_get_int(structure, "height", &frame.height);

            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ))
            {
                frame.pixels = map.data;

                std::lock_guard<std::mutex> lock(impl->callbackMtx);
                if (impl->frameCallback)
                    impl->frameCallback(frame);

                gst_buffer_unmap(buffer, &map);
            }
        }

        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    // Binds an appsink (found by name in a manually specified pipeline) to our
    // video/audio sample callbacks.
    bool configureNamedAppsink(GstElement *pipeline, const char *name, GstFlowReturn (*callback)(GstElement *, gpointer), gpointer userData)
    {
        GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), name);
        if (!sink)
            return false;

        g_object_set(sink, "emit-signals", TRUE, "sync", FALSE, nullptr);
        g_signal_connect(sink, "new-sample", G_CALLBACK(callback), userData);
        gst_object_unref(sink);
        return true;
    }

    // Rough heuristic for "is a raw gst-launch pipeline string" vs a plain
    // media URI/path. Manual pipelines are recognizable by element syntax.
    bool looksLikePipelineString(const std::string &source)
    {
        return source.find(" ! ") != std::string::npos ||
               source.find("udpsrc") != std::string::npos ||
               source.find("rtpbin") != std::string::npos;
    }

    // Builds a playbin-friendly URI from a path or URL. Relative paths are
    // resolved to absolute paths first (file://foo/bar.mp4 is invalid).
    std::string toMediaUri(const std::string &source)
    {
        if (source.find("://") != std::string::npos)
            return source;

        std::string path = source;
        if (!path.empty() && path.front() == '/')
        {
            return "file://" + path;
        }

        std::error_code ec;
        const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
        if (ec || path.empty())
        {
            return "file://" + path;
        }
        return "file://" + absolute.lexically_normal().string();
    }

    // Tears down and releases any pipeline currently held by the player.
    void cleanupPipeline(SimpleMedia::VideoPlayer::Impl *impl)
    {
        if (impl->pipeline)
        {
            gst_element_set_state(impl->pipeline, GST_STATE_NULL);
            gst_object_unref(impl->pipeline);
            impl->pipeline = nullptr;
        }
    }
} // namespace

namespace SimpleMedia
{
    VideoPlayer::VideoPlayer() : pImpl(std::make_unique<Impl>())
    {
        // Initialise GStreamer exactly once, without touching the command line.
        static std::once_flag initFlag;
        std::call_once(initFlag, []()
                       { gst_init(nullptr, nullptr); });
    }

    VideoPlayer::VideoPlayer(VideoPlayer &&) noexcept = default;
    VideoPlayer &VideoPlayer::operator=(VideoPlayer &&) noexcept = default;

    VideoPlayer::~VideoPlayer()
    {
        if (pImpl)
        {
            cleanupPipeline(pImpl.get());
        }
    }

    void VideoPlayer::setFrameCallback(std::function<void(const VideoFrame &)> callback)
    {
        std::lock_guard<std::mutex> lock(pImpl->callbackMtx);
        pImpl->frameCallback = std::move(callback);
    }

    void VideoPlayer::setAudioCallback(std::function<void(const AudioFrame &)> callback)
    {
        std::lock_guard<std::mutex> lock(pImpl->callbackMtx);
        pImpl->audioCallback = std::move(callback);
    }

    bool VideoPlayer::load(const std::string &source)
    {
        // Never leak a previously configured pipeline.
        cleanupPipeline(pImpl.get());

        if (looksLikePipelineString(source))
        {
            std::printf("[SimpleMedia] Configuring manual pipeline: %s\n", source.c_str());

            GError *error = nullptr;
            pImpl->pipeline = gst_parse_launch(source.c_str(), &error);
            if (!pImpl->pipeline)
            {
                std::fprintf(stderr, "[SimpleMedia] Failed to parse custom pipeline: %s\n",
                             error ? error->message : "unknown error");
                if (error)
                    g_error_free(error);
                return false;
            }

            // Connect any appsinks named `videosink` / `audiosink` found in
            // the pipeline description to the sample callbacks.
            configureNamedAppsink(pImpl->pipeline, "videosink", on_new_sample, pImpl.get());
            configureNamedAppsink(pImpl->pipeline, "audiosink", on_new_audio_sample, pImpl.get());
            return true;
        }

        // -----------------------------------------------------------------
        // Standard playbin path for files and HTTP/RTSP URLs.
        // -----------------------------------------------------------------
        pImpl->pipeline = gst_element_factory_make("playbin", "player");
        if (!pImpl->pipeline)
            return false;

        gst_util_set_object_arg(G_OBJECT(pImpl->pipeline), "flags", kPlaybinFlags);

        // Video appsink: decoded frames are delivered to us as RGB.
        GstElement *videoSink = gst_element_factory_make("appsink", "videosink");
        GstCaps *videoCaps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", nullptr);
        g_object_set(videoSink, "caps", videoCaps, "emit-signals", TRUE, "sync", TRUE, nullptr);
        gst_caps_unref(videoCaps);
        g_signal_connect(videoSink, "new-sample", G_CALLBACK(on_new_sample), pImpl.get());

        // Audio appsink: decoded audio is delivered to us as interleaved S16LE.
        GstElement *audioSink = gst_element_factory_make("appsink", "audiosink");
        GstCaps *audioCaps = gst_caps_new_simple("audio/x-raw",
                                                 "format", G_TYPE_STRING, "S16LE",
                                                 "layout", G_TYPE_STRING, "interleaved", nullptr);
        g_object_set(audioSink, "caps", audioCaps, "emit-signals", TRUE, "sync", TRUE, nullptr);
        gst_caps_unref(audioCaps);
        g_signal_connect(audioSink, "new-sample", G_CALLBACK(on_new_audio_sample), pImpl.get());

        g_object_set(pImpl->pipeline,
                     "uri", toMediaUri(source).c_str(),
                     "video-sink", videoSink,
                     "audio-sink", audioSink,
                     nullptr);

        // Pre-roll so playback starts instantly on the first call to play().
        // ASYNC (live/network sources) is fine; only an outright FAILURE is fatal.
        GstStateChangeReturn ret = gst_element_set_state(pImpl->pipeline, GST_STATE_PAUSED);
        if (ret == GST_STATE_CHANGE_FAILURE)
        {
            std::fprintf(stderr, "[SimpleMedia] Failed to pre-roll pipeline.\n");
            cleanupPipeline(pImpl.get());
            return false;
        }

        GstState state = GST_STATE_PAUSED;
        gst_element_get_state(pImpl->pipeline, &state, nullptr, 2 * GST_SECOND);
        if (state == GST_STATE_NULL)
        {
            std::fprintf(stderr, "[SimpleMedia] Pipeline entered NULL state during pre-roll.\n");
            cleanupPipeline(pImpl.get());
            return false;
        }

        return true;
    }

    void VideoPlayer::play()
    {
        if (pImpl->pipeline)
            gst_element_set_state(pImpl->pipeline, GST_STATE_PLAYING);
    }

    void VideoPlayer::pause()
    {
        if (pImpl->pipeline)
            gst_element_set_state(pImpl->pipeline, GST_STATE_PAUSED);
    }

    void VideoPlayer::stop()
    {
        if (pImpl->pipeline)
            gst_element_set_state(pImpl->pipeline, GST_STATE_NULL);
    }

    void VideoPlayer::setVolume(double volume)
    {
        // The volume property only exists on playbin-style pipelines, not on
        // arbitrary manually parsed pipelines.
        if (pImpl->pipeline &&
            g_object_class_find_property(G_OBJECT_GET_CLASS(pImpl->pipeline), "volume"))
        {
            g_object_set(pImpl->pipeline, "volume", volume, nullptr);
        }
    }
} // namespace SimpleMedia