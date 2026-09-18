#include "SimpleMedia/SimpleMedia.h"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <cstdio>
#include <cstdint>
#include <memory>
#include <mutex>

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
        if (pImpl->pipeline)
            gst_element_set_state(pImpl->pipeline, GST_STATE_NULL);
    }
} // namespace SimpleMedia