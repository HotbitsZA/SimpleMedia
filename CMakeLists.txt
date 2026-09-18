cmake_minimum_required(VERSION 3.16)
project(SimpleMedia VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(SIMPLEMEDIA_BUILD_EXAMPLES "Build the bundled example applications" ON)
option(SIMPLEMEDIA_BUILD_GUI_EXAMPLES "Build the Dear ImGui examples (requires GLFW + ImGui)" ON)
set(SIMPLEMEDIA_IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../OpenSource/imgui"
    CACHE PATH "Path to the Dear ImGui source tree (must contain imgui/ + backends/)")

# ---------------------------------------------------------------------------
# GStreamer (system packages)
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-audio-1.0
    gstreamer-video-1.0
)

# ---------------------------------------------------------------------------
# SimpleMedia library
# ---------------------------------------------------------------------------
add_library(SimpleMedia STATIC
    src/VideoPlayer.cpp
    src/VideoStreamer.cpp
    src/AudioOutput.cpp
    src/VideoOutput.cpp
    src/MainLoop.cpp
)
target_include_directories(SimpleMedia PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
)
target_link_libraries(SimpleMedia PUBLIC PkgConfig::GSTREAMER)
target_compile_options(SimpleMedia PRIVATE -Wall -Wextra)
if(APPLE)
    target_link_libraries(SimpleMedia PUBLIC "-framework Cocoa")
endif()

# ---------------------------------------------------------------------------
# Examples
# ---------------------------------------------------------------------------
if(SIMPLEMEDIA_BUILD_EXAMPLES)
    # CLI examples (no extra dependencies beyond GStreamer).
    add_executable(play_app examples/Play.cpp)
    target_link_libraries(play_app PRIVATE SimpleMedia)

    add_executable(stream_app examples/Stream.cpp)
    target_link_libraries(stream_app PRIVATE SimpleMedia)

    add_executable(combined_demo examples/main.cpp)
    target_link_libraries(combined_demo PRIVATE SimpleMedia)

    # GUI examples (Dear ImGui + GLFW + OpenGL). Only built when available.
    if(SIMPLEMEDIA_BUILD_GUI_EXAMPLES)
        pkg_check_modules(GLFW IMPORTED_TARGET glfw3)

        set(IMGUI_VALID FALSE)
        if(GLFW_FOUND AND EXISTS "${SIMPLEMEDIA_IMGUI_DIR}/imgui.cpp")
            set(IMGUI_VALID TRUE)
        endif()

        if(IMGUI_VALID)
            set(IMGUI_SOURCES
                "${SIMPLEMEDIA_IMGUI_DIR}/imgui.cpp"
                "${SIMPLEMEDIA_IMGUI_DIR}/imgui_draw.cpp"
                "${SIMPLEMEDIA_IMGUI_DIR}/imgui_tables.cpp"
                "${SIMPLEMEDIA_IMGUI_DIR}/imgui_widgets.cpp"
                "${SIMPLEMEDIA_IMGUI_DIR}/backends/imgui_impl_glfw.cpp"
                "${SIMPLEMEDIA_IMGUI_DIR}/backends/imgui_impl_opengl3.cpp"
            )

            find_package(OpenGL REQUIRED)

            # Shared setup for the ImGui + GLFW + OpenGL examples.
            function(configure_gui_target target)
                target_include_directories(${target} SYSTEM PRIVATE
                    ${SIMPLEMEDIA_IMGUI_DIR}
                    ${SIMPLEMEDIA_IMGUI_DIR}/backends
                )
                target_compile_options(${target} PRIVATE -Wno-nontrivial-memcall)
                target_link_libraries(${target} PRIVATE
                    SimpleMedia
                    PkgConfig::GLFW
                    OpenGL::GL
                )
                if(APPLE)
                    target_link_libraries(${target} PRIVATE
                        "-framework Cocoa"
                        "-framework IOKit"
                        "-framework CoreVideo"
                    )
                    target_compile_definitions(${target} PRIVATE GL_SILENCE_DEPRECATION)
                endif()
            endfunction()

            add_executable(gui_player_app examples/GuiApp.cpp ${IMGUI_SOURCES})
            configure_gui_target(gui_player_app)

            add_executable(gui_player_basic_app examples/ImguiPlay.cpp ${IMGUI_SOURCES})
            configure_gui_target(gui_player_basic_app)
        else()
            message(WARNING "Dear ImGui GUI examples skipped. "
                            "Install glfw3 and set SIMPLEMEDIA_IMGUI_DIR to your ImGui tree " 
                            "(currently \"${SIMPLEMEDIA_IMGUI_DIR}\").")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Install (make it consumable via add_subdirectory or find_package)
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS SimpleMedia
    EXPORT SimpleMediaTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(FILES
    include/SimpleMedia/SimpleMedia.h
    include/SimpleMedia/AudioRingBuffer.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/SimpleMedia
)
install(EXPORT SimpleMediaTargets
    FILE SimpleMediaTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/SimpleMedia
)