#include "SimpleMedia/SimpleMedia.h"

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <atomic>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

namespace SimpleMedia
{
    struct VideoStreamer::Impl
    {
        GstElement *pipeline = nullptr;
        GstElement *appsrc = nullptr;   // Video source
        GstElement *audiosrc = nullptr; // Audio source

        int width = 0;
        int height = 0;

        // Independent presentation timestamps to keep the two timelines flowing.
        GstClockTime videoTimestamp = 0;
        GstClockTime audioTimestamp = 0;

        // Last time audio was fed (in video timestamps). When a frame is pushed
        // but audio has gone quiet, silence is synthesised so the live pipeline
        // never stalls waiting on a starved audio branch.
        GstClockTime lastAudioFeedTime = 0;
        std::mutex pushMutex;

        // Async source streaming (startStreamingSource) support.
        std::thread sourceThread;      // Worker draining a source into the stream
        std::thread::id sourceThreadId; // Id of that worker (used to avoid self-join)
        std::atomic<bool> cancelRequested{false};
        std::function<void(bool)> doneCallback;
    };
} // namespace SimpleMedia

namespace
{
    // Hard bounds for the appsrc queues. When the downstream sink is slower
    // than the producer, push calls block instead of growing memory without
    // limit. 4 MiB of raw frames is ~15 frames of 1080p RGBA.
    constexpr guint kAppSrcMaxBytes = 4 * 1024 * 1024;

    // Synthesised silence block (20 ms of stereo S16LE = 882 frames * 4 bytes).
    constexpr size_t kSilenceBytes = 882 * 4;

    void cleanupPipeline(SimpleMedia::VideoStreamer::Impl *impl)
    {
        if (impl->pipeline)
        {
            gst_element_set_state(impl->pipeline, GST_STATE_NULL);
            gst_object_unref(impl->pipeline);
            impl->pipeline = nullptr;
            impl->appsrc = nullptr;
            impl->audiosrc = nullptr;
        }
    }

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

    // Forwards decoded video frames straight into the streamer.
    GstFlowReturn on_source_video_sample(GstElement *sink, gpointer data)
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

    // Forwards decoded audio blocks straight into the streamer.
    GstFlowReturn on_source_audio_sample(GstElement *sink, gpointer data)
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

    // A plain appsink directly usable as a playbin sink. playbin's internal
    // playsink inserts the converters it needs, so enforcing the streamer's
    // exact RGBA frame size is enough here (bins with ghost pads do NOT link).
    GstElement *makeSourceVideoSink(SimpleMedia::VideoStreamer *streamer, int width, int height)
    {
        GstElement *sink = gst_element_factory_make("appsink", "v_source_sink");
        if (!sink)
            return nullptr;

        GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                            "format", G_TYPE_STRING, "RGBA",
                                            "width", G_TYPE_INT, width,
                                            "height", G_TYPE_INT, height, nullptr);
        g_object_set(sink, "caps", caps, "emit-signals", TRUE, "sync", TRUE, nullptr);
        gst_caps_unref(caps);
        g_signal_connect(sink, "new-sample", G_CALLBACK(on_source_video_sample), streamer);
        return sink;
    }

    GstElement *makeSourceAudioSink(SimpleMedia::VideoStreamer *streamer)
    {
        GstElement *sink = gst_element_factory_make("appsink", "a_source_sink");
        if (!sink)
            return nullptr;

        GstCaps *caps = gst_caps_new_simple("audio/x-raw",
                                            "format", G_TYPE_STRING, "S16LE",
                                            "layout", G_TYPE_STRING, "interleaved",
                                            "rate", G_TYPE_INT, 44100,
                                            "channels", G_TYPE_INT, 2, nullptr);
        g_object_set(sink, "caps", caps, "emit-signals", TRUE, "sync", TRUE, nullptr);
        gst_caps_unref(caps);
        g_signal_connect(sink, "new-sample", G_CALLBACK(on_source_audio_sample), streamer);
        return sink;
    }

    void teardownSourceReader(GstElement *filePipeline)
    {
        if (filePipeline)
        {
            gst_element_set_state(filePipeline, GST_STATE_NULL);
            gst_object_unref(filePipeline);
        }
    }

    // Result of streaming a source end to end once.
    struct SourceStreamResult
    {
        bool streamedFully = false; // Reached clean EOS
        bool cancelled = false;     // Cancelled via stop()/destructor
    };

    // Poll period for the reader bus loop, so the cancel flag is re-checked
    // even while a source is stalled (no EOS/error message ever arrives).
    constexpr GstClockTime kSourcePollPeriod = 100 * GST_MSECOND;

    // Joins the async source worker, requesting cancellation first. Skipped
    // when called from the worker thread itself (e.g. stop() inside the
    // onFinished callback): the stream has ended by then, so there is nothing
    // real to wait for.
    void cancelAndJoinSource(SimpleMedia::VideoStreamer::Impl *impl)
    {
        if (impl->sourceThread.joinable() &&
            std::this_thread::get_id() != impl->sourceThreadId)
        {
            impl->cancelRequested.store(true);
            impl->sourceThread.join();
        }
    }

    // Runs the full source -> decode -> encode -> RTP path once. Shared by the
    // blocking streamSource() and the async startStreamingSource() worker.
    // Tears the reader down and stops the encoder before returning.
    SourceStreamResult runSourceStreaming(SimpleMedia::VideoStreamer::Impl *impl,
                                          SimpleMedia::VideoStreamer *streamer,
                                          const std::string &source,
                                          const std::string &destinationIp,
                                          int videoPort, int audioPort,
                                          int width, int height)
    {
        impl->cancelRequested.store(false);

        // Encoder half: RGBA/S16LE -> H.264/Opus -> RTP.
        if (!streamer->setupStream(destinationIp, videoPort, audioPort, width, height))
        {
            std::fprintf(stderr, "[SimpleMedia] Failed to set up the streaming pipeline.\n");
            return {};
        }
        streamer->start();

        // Reading half: a playbin (decodes any source) whose sinks forward into
        // the encoder appsrcs. playbin inserts the converters needed to match
        // the streamer's exact RGBA / S16LE caps.
        GstElement *filePipeline = gst_element_factory_make("playbin", "file_reader");
        if (!filePipeline)
        {
            std::fprintf(stderr, "[SimpleMedia] Failed to create the source reader pipeline.\n");
            if (impl->pipeline)
                gst_element_set_state(impl->pipeline, GST_STATE_NULL);
            return {};
        }
        gst_util_set_object_arg(G_OBJECT(filePipeline), "flags", "video+audio");

        GstElement *videoSink = makeSourceVideoSink(streamer, width, height);
        GstElement *audioSink = makeSourceAudioSink(streamer);
        if (!videoSink || !audioSink)
        {
            std::fprintf(stderr, "[SimpleMedia] Failed to build the source reader sinks.\n");
            teardownSourceReader(filePipeline);
            if (impl->pipeline)
                gst_element_set_state(impl->pipeline, GST_STATE_NULL);
            return {};
        }

        g_object_set(filePipeline,
                     "uri", toMediaUri(source).c_str(),
                     "video-sink", videoSink,
                     "audio-sink", audioSink,
                     nullptr);

        GstStateChangeReturn ret = gst_element_set_state(filePipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE)
        {
            std::fprintf(stderr, "[SimpleMedia] Failed to start reading \"%s\".\n", source.c_str());
            teardownSourceReader(filePipeline);
            if (impl->pipeline)
                gst_element_set_state(impl->pipeline, GST_STATE_NULL);
            return {};
        }

        // Run until the source finishes (EOS), errors, or cancellation. The
        // timeout keeps the loop responsive to the cancel flag.
        GstBus *bus = gst_element_get_bus(filePipeline);
        SourceStreamResult result;
        while (!impl->cancelRequested.load())
        {
            GstMessage *msg = gst_bus_timed_pop_filtered(
                bus, kSourcePollPeriod,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
            if (!msg)
                continue; // Timeout: re-check the cancel flag.

            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR)
            {
                GError *err = nullptr;
                gchar *debug = nullptr;
                gst_message_parse_error(msg, &err, &debug);
                std::fprintf(stderr, "[SimpleMedia] Source read error: %s\n", err->message);
                if (debug)
                    std::fprintf(stderr, "  (debug info: %s)\n", debug);
                g_error_free(err);
                g_free(debug);
            }
            else
            {
                result.streamedFully = true; // GST_MESSAGE_EOS
            }
            gst_message_unref(msg);
            break;
        }
        result.cancelled = impl->cancelRequested.load();

        gst_object_unref(bus);
        teardownSourceReader(filePipeline);

        // Stop the encoder directly instead of going through stop(), which
        // involves thread joining that is unsafe from the worker thread.
        if (impl->pipeline)
            gst_element_set_state(impl->pipeline, GST_STATE_NULL);

        return result;
    }
} // namespace

namespace SimpleMedia
{
    VideoStreamer::VideoStreamer() : pImpl(std::make_unique<Impl>())
    {
        static std::once_flag initFlag;
        std::call_once(initFlag, []()
                       { gst_init(nullptr, nullptr); });
    }

    VideoStreamer::VideoStreamer(VideoStreamer &&) noexcept = default;
    VideoStreamer &VideoStreamer::operator=(VideoStreamer &&) noexcept = default;

    VideoStreamer::~VideoStreamer()
    {
        if (pImpl)
        {
            if (pImpl->sourceThread.joinable())
            {
                pImpl->cancelRequested.store(true);
                pImpl->sourceThread.join();
            }
            cleanupPipeline(pImpl.get());
        }
    }

    bool VideoStreamer::setupStream(const std::string &destinationIp, int videoPort, int audioPort,
                                    int width, int height)
    {
        // Release any stream configured previously.
        cleanupPipeline(pImpl.get());

        pImpl->width = width;
        pImpl->height = height;
        pImpl->videoTimestamp = 0;
        pImpl->audioTimestamp = 0;

        GstElement *pipeline = gst_pipeline_new("dual-streamer-pipeline");

        // -----------------------------------------------------------------
        // Branch 1: Video encoding & network routing (RGBA -> H.264 -> RTP).
        // -----------------------------------------------------------------
        GstElement *appsrc = gst_element_factory_make("appsrc", "video_source");
        GstElement *v_conv = gst_element_factory_make("videoconvert", "video_convert");
        GstElement *v_enc = gst_element_factory_make("x264enc", "video_encoder");
        if (!v_enc)
        {
            // Fall back to VideoToolbox (Apple Silicon / macOS).
            v_enc = gst_element_factory_make("vtenc_h264", "video_encoder");
        }
        GstElement *v_pay = gst_element_factory_make("rtph264pay", "video_payload");
        GstElement *v_sink = gst_element_factory_make("udpsink", "video_sink");

        // -----------------------------------------------------------------
        // Branch 2: Audio encoding & network routing (S16LE -> Opus -> RTP).
        // -----------------------------------------------------------------
        GstElement *audiosrc = gst_element_factory_make("appsrc", "audio_source");
        GstElement *a_conv = gst_element_factory_make("audioconvert", "audio_convert");
        GstElement *a_resam = gst_element_factory_make("audioresample", "audio_resample");
        GstElement *a_enc = gst_element_factory_make("opusenc", "audio_encoder");
        GstElement *a_pay = gst_element_factory_make("rtpopuspay", "audio_payload");
        GstElement *a_sink = gst_element_factory_make("udpsink", "audio_sink");

        if (!pipeline || !appsrc || !v_conv || !v_enc || !v_pay || !v_sink ||
            !audiosrc || !a_conv || !a_resam || !a_enc || !a_pay || !a_sink)
        {
            std::fprintf(stderr, "[SimpleMedia] Failed to create one or more pipeline elements.\n");
            if (pipeline)
                gst_object_unref(pipeline);
            return false;
        }

        // Video appsrc caps: raw RGBA at the requested resolution, 30 FPS.
        GstCaps *videoCaps = gst_caps_new_simple("video/x-raw",
                                                 "format", G_TYPE_STRING, "RGBA",
                                                 "width", G_TYPE_INT, width,
                                                 "height", G_TYPE_INT, height,
                                                 "framerate", GST_TYPE_FRACTION, 30, 1, nullptr);
        g_object_set(appsrc, "caps", videoCaps, "format", GST_FORMAT_TIME,
                     "is-live", TRUE, "block", FALSE, "max-bytes", kAppSrcMaxBytes, nullptr);
        gst_caps_unref(videoCaps);

        // Audio appsrc caps: interleaved S16LE stereo @ 44.1 kHz.
        GstCaps *audioCaps = gst_caps_new_simple("audio/x-raw",
                                                 "format", G_TYPE_STRING, "S16LE",
                                                 "layout", G_TYPE_STRING, "interleaved",
                                                 "rate", G_TYPE_INT, 44100,
                                                 "channels", G_TYPE_INT, 2, nullptr);
        g_object_set(audiosrc, "caps", audioCaps, "format", GST_FORMAT_TIME,
                     "is-live", TRUE, "block", FALSE, "max-bytes", kAppSrcMaxBytes, nullptr);
        gst_caps_unref(audioCaps);

        // Network sinks: low-latency live streaming, no sync to a clock.
        g_object_set(v_sink, "host", destinationIp.c_str(), "port", videoPort, "sync", FALSE, nullptr);
        g_object_set(a_sink, "host", destinationIp.c_str(), "port", audioPort, "sync", FALSE, nullptr);

        if (g_object_class_find_property(G_OBJECT_GET_CLASS(v_enc), "tune"))
        {
            g_object_set(v_enc, "tune", 0x00000004, nullptr); // Zero-latency tuning
        }

        // Force a keyframe every second (30 frames @ 30 FPS). Combined with
        // config-interval below, a receiver joining mid-stream decodes quickly.
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(v_enc), "key-int-max"))
        {
            g_object_set(v_enc, "key-int-max", 30, nullptr);
        }

        // Repeat SPS/PPS with every IDR frame so receivers that join mid-stream
        // (or drop a keyframe) can still decode. "1" = config with every IDR.
        g_object_set(v_pay, "config-interval", 1, nullptr);

        // Assemble everything into the master pipeline.
        gst_bin_add_many(GST_BIN(pipeline),
                         appsrc, v_conv, v_enc, v_pay, v_sink,
                         audiosrc, a_conv, a_resam, a_enc, a_pay, a_sink,
                         nullptr);

        const bool videoLinked = gst_element_link_many(appsrc, v_conv, v_enc, v_pay, v_sink, nullptr);
        const bool audioLinked = gst_element_link_many(audiosrc, a_conv, a_resam, a_enc, a_pay, a_sink, nullptr);

        if (!videoLinked || !audioLinked)
        {
            std::fprintf(stderr, "[SimpleMedia] Failed to link pipeline branches (video=%d, audio=%d).\n",
                         videoLinked, audioLinked);
            gst_object_unref(pipeline);
            return false;
        }

        pImpl->pipeline = pipeline;
        pImpl->appsrc = appsrc;
        pImpl->audiosrc = audiosrc;
        return true;
    }

    void VideoStreamer::pushFrame(const uint8_t *rgbaData)
    {
        if (!pImpl->appsrc || !rgbaData)
            return;

        const size_t size = static_cast<size_t>(pImpl->width) * static_cast<size_t>(pImpl->height) * 4;
        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
        if (!buffer)
            return;

        gst_buffer_fill(buffer, 0, rgbaData, size);

        // 30 FPS pacing.
        const GstClockTime duration = gst_util_uint64_scale_int(1, GST_SECOND, 30);

        std::lock_guard<std::mutex> lock(pImpl->pushMutex);

        // Keep the audio branch from stalling the whole live pipeline. If real
        // audio has not been fed recently, synthesise a short silence block so
        // every branch stays in PLAYING regardless of what the caller sends.
        if (pImpl->audiosrc &&
            (pImpl->lastAudioFeedTime == 0 ||
             pImpl->videoTimestamp - pImpl->lastAudioFeedTime > 3 * duration))
        {
            GstBuffer *silence = gst_buffer_new_allocate(nullptr, kSilenceBytes, nullptr);
            if (silence)
            {
                gst_buffer_memset(silence, 0, 0, kSilenceBytes);
                const GstClockTime silenceDuration =
                    gst_util_uint64_scale_int(kSilenceBytes / 4, GST_SECOND, 44100);
                GST_BUFFER_PTS(silence) = pImpl->audioTimestamp;
                GST_BUFFER_DURATION(silence) = silenceDuration;
                pImpl->audioTimestamp += silenceDuration;
                gst_app_src_push_buffer(GST_APP_SRC(pImpl->audiosrc), silence);
            }
            pImpl->lastAudioFeedTime = pImpl->videoTimestamp;
        }

        GST_BUFFER_PTS(buffer) = pImpl->videoTimestamp;
        GST_BUFFER_DURATION(buffer) = duration;
        pImpl->videoTimestamp += duration;

        gst_app_src_push_buffer(GST_APP_SRC(pImpl->appsrc), buffer);
    }

    void VideoStreamer::pushAudio(const uint8_t *pcmData, size_t dataSize)
    {
        if (!pImpl->audiosrc || !pcmData || dataSize == 0)
            return;

        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, dataSize, nullptr);
        if (!buffer)
            return;

        gst_buffer_fill(buffer, 0, pcmData, dataSize);

        std::lock_guard<std::mutex> lock(pImpl->pushMutex);

        // Duration for interleaved S16LE stereo (4 bytes per sample frame).
        const size_t totalFrames = dataSize / 4;
        const GstClockTime duration = gst_util_uint64_scale_int(totalFrames, GST_SECOND, 44100);
        GST_BUFFER_PTS(buffer) = pImpl->audioTimestamp;
        GST_BUFFER_DURATION(buffer) = duration;
        pImpl->audioTimestamp += duration;

        gst_app_src_push_buffer(GST_APP_SRC(pImpl->audiosrc), buffer);
        pImpl->lastAudioFeedTime = pImpl->videoTimestamp;
    }

    void VideoStreamer::start()
    {
        if (pImpl->pipeline)
            gst_element_set_state(pImpl->pipeline, GST_STATE_PLAYING);
    }

    void VideoStreamer::stop()
    {
        // Cancels any in-flight async source stream before stopping the encoder
        // beneath it. Safe to call from within the onFinished callback as well.
        cancelAndJoinSource(pImpl.get());
        if (pImpl->pipeline)
            gst_element_set_state(pImpl->pipeline, GST_STATE_NULL);
    }

    bool VideoStreamer::streamSource(const std::string &source,
                                     const std::string &destinationIp,
                                     int videoPort, int audioPort,
                                     int width, int height)
    {
        const SourceStreamResult result = runSourceStreaming(pImpl.get(), this, source,
                                                             destinationIp, videoPort,
                                                             audioPort, width, height);
        return result.streamedFully;
    }

    void VideoStreamer::startStreamingSource(const std::string &source,
                                             const std::string &destinationIp,
                                             int videoPort, int audioPort,
                                             std::function<void(bool)> onFinished,
                                             int width, int height)
    {
        // Shut down any previous source stream before launching a new one.
        cancelAndJoinSource(pImpl.get());
        pImpl->doneCallback = std::move(onFinished);

        pImpl->sourceThread = std::thread([this, source, destinationIp, videoPort,
                                           audioPort, width, height]()
        {
            // Record our identity first so stop()/startStreamingSource()/the
            // destructor never try to join us from within ourselves.
            pImpl->sourceThreadId = std::this_thread::get_id();

            const SourceStreamResult result =
                runSourceStreaming(pImpl.get(), this, source, destinationIp,
                                   videoPort, audioPort, width, height);

            std::function<void(bool)> callback;
            {
                std::lock_guard<std::mutex> lock(pImpl->pushMutex);
                callback = std::move(pImpl->doneCallback);
                pImpl->doneCallback = nullptr;
            }
            if (callback)
                callback(result.streamedFully);
        });
    }
} // namespace SimpleMedia