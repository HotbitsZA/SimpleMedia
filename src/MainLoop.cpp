#include "SimpleMedia/SimpleMedia.h"

#if defined(__APPLE__)
#include <gst/gst.h>
#include <gst/gstmacos.h>
#endif

namespace SimpleMedia
{
    namespace
    {
#if defined(__APPLE__)
        struct MacMainArgs
        {
            MainFunction func;
            void *userData;
        };

        // Matches GstMainFuncSimple so gst_macos_main_simple can hand off to us.
        int macMainDispatch(void *data)
        {
            auto *m = static_cast<MacMainArgs *>(data);
            return m->func(m->userData);
        }
#endif
    } // namespace

    int runMain(MainFunction func, void *userData)
    {
#if defined(__APPLE__)
        // gst_macos_main() starts NSApplication on the main thread and pumps
        // the Cocoa main loop while `func` runs on a worker thread, which is
        // what the GL/Cocoa video sinks require.
        MacMainArgs args{func, userData};
        return gst_macos_main_simple(&macMainDispatch, &args);
#else
        return func(userData);
#endif
    }
} // namespace SimpleMedia