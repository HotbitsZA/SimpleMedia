#include "SimpleMedia/SimpleMedia.h"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <cstdio>
#include <cstring>
#include <mutex>

#if defined(__APPLE__)
#include <objc/message.h>
#include <objc/runtime.h>
#include <pthread.h>
#endif

namespace SimpleMedia
{
    struct VideoOutput::Impl
    {
        GstElement *pipeline = nullptr;
        GstElement *appsrc = nullptr;
        int width = 0;    // Caps of the last frame we pushed
        int height = 0;
        std::mutex mtx;   // Guards pipeline use across threads
    };
} // namespace SimpleMedia

namespace
{
    // autovideosink opens a native window on the default display. A tiny queue
    // smooths out frame-pacing jitter without adding noticeable latency; the
    // player appsink already paces frames in real time.
    constexpr const char *kOutputPipeline =
        "appsrc name=src format=time is-live=true do-timestamp=true "
        "! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 "
        "! videoconvert ! autovideosink";

#if defined(__APPLE__)
    // macOS GL/Cocoa video sinks need an NSApplication on the main thread.
    // Done here via the C ObjC runtime so the library stays compilable as
    // plain C++ (AppKit headers are not valid in a .cpp translation unit).
    void ensureMainThreadApplication()
    {
        if (!pthread_main_np())
            return;

        Class nsApplication = objc_getClass("NSApplication");
        if (nsApplication)
        {
            using SharedApplicationFn = id (*)(id, SEL);
            SharedApplicationFn sharedApplication =
                reinterpret_cast<SharedApplicationFn>(objc_msgSend);
            sharedApplication(reinterpret_cast<id>(nsApplication),
                              sel_registerName("sharedApplication"));
        }
    }
#endif
} // namespace

namespace SimpleMedia
{
    VideoOutput::VideoOutput() : pImpl(std::make_unique<Impl>())
    {
        // Initialise GStreamer exactly once, without touching the command line.
        static std::once_flag initFlag;
        std::call_once(initFlag, []()
                       { gst_init(nullptr, nullptr); });
    }

    VideoOutput::VideoOutput(VideoOutput &&) noexcept = default;
    VideoOutput &VideoOutput::operator=(VideoOutput &&) noexcept = default;

    VideoOutput::~VideoOutput()
    {
        if (pImpl)
        {
            stop();
        }
    }

    bool VideoOutput::open()
    {
        stop();

#if defined(__APPLE__)
        // If the app is running us on the main thread (rather than inside
        // SimpleMedia::runMain() which already sets this up), create the app
        // object so the GL/Cocoa sinks have somewhere to attach their window.
        ensureMainThreadApplication();
#endif

        GstElement *pipeline = gst_parse_launch(kOutputPipeline, nullptr);
        if (!pipeline)
        {
            std::fprintf(stderr, "[SimpleMedia] VideoOutput: failed to build the "
                                 "video output pipeline.\n");
            return false;
        }

        GstElement *appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
        if (!appsrc || gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
                             GST_STATE_CHANGE_FAILURE)
        {
            if (appsrc)
                gst_object_unref(appsrc);
            gst_object_unref(pipeline);
            std::fprintf(stderr, "[SimpleMedia] VideoOutput: no usable display "
                                 "device.\n");
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(pImpl->mtx);
            pImpl->pipeline = pipeline;
            pImpl->appsrc = appsrc;
        }
        return true;
    }

    void VideoOutput::write(const VideoFrame &frame)
    {
        if (!frame.pixels || frame.width <= 0 || frame.height <= 0)
            return;

        const size_t frameSize =
            static_cast<size_t>(frame.width) * static_cast<size_t>(frame.height) * 3;

        std::lock_guard<std::mutex> lock(pImpl->mtx);
        if (!pImpl->pipeline || !pImpl->appsrc)
            return;

        if (pImpl->width != frame.width || pImpl->height != frame.height)
        {
            GstCaps *caps = gst_caps_new_simple("video/x-raw",
                                                "format", G_TYPE_STRING, "RGB",
                                                "width", G_TYPE_INT, frame.width,
                                                "height", G_TYPE_INT, frame.height,
                                                nullptr);
            g_object_set(pImpl->appsrc, "caps", caps, nullptr);
            gst_caps_unref(caps);
            pImpl->width = frame.width;
            pImpl->height = frame.height;
        }

        GstBuffer *buffer = gst_buffer_new_allocate(nullptr, frameSize, nullptr);
        if (!buffer)
            return;

        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_WRITE))
        {
            std::memcpy(map.data, frame.pixels, frameSize);
            gst_buffer_unmap(buffer, &map);
        }

        const GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(pImpl->appsrc), buffer);
        if (ret != GST_FLOW_OK && ret != GST_FLOW_FLUSHING)
        {
            std::fprintf(stderr, "[SimpleMedia] VideoOutput: push failed "
                                 "(flow=%s).\n", gst_flow_get_name(ret));
        }
    }

    void VideoOutput::stop()
    {
        std::lock_guard<std::mutex> lock(pImpl->mtx);
        if (pImpl->appsrc)
        {
            gst_object_unref(pImpl->appsrc);
            pImpl->appsrc = nullptr;
        }
        if (pImpl->pipeline)
        {
            gst_element_set_state(pImpl->pipeline, GST_STATE_NULL);
            gst_object_unref(pImpl->pipeline);
            pImpl->pipeline = nullptr;
        }
        pImpl->width = 0;
        pImpl->height = 0;
    }
} // namespace SimpleMedia