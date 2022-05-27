cmake_minimum_required(VERSION 3.18)
project(gstreamer)

set(CMAKE_CXX_STANDARD 20)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK3 REQUIRED gtk+-3.0)
pkg_search_module(GLIB REQUIRED glib-2.0)
pkg_search_module(SOUP REQUIRED libsoup-2.4)
pkg_search_module(JSON_GLIB REQUIRED json-glib-1.0)

pkg_check_modules(GST REQUIRED
    gstreamer-1.0>=1.20
    gstreamer-sdp-1.0>=1.20
    gstreamer-app-1.0>=1.20
    gstreamer-audio-1.0>=1.20
    gstreamer-video-1.0>=1.20
    gstreamer-webrtc-1.0>=1.20
)


add_executable(gstreamer webrtc.cpp custom.cpp)
target_include_directories(gstreamer PRIVATE ${GTK3_INCLUDE_DIRS} ${GST_INCLUDE_DIRS} ${SOUP_INCLUDE_DIRS} ${JSON_GLIB_INCLUDE_DIRS})
target_link_libraries(gstreamer ${GTK3_LIBRARIES} ${GST_LIBRARIES} ${SOUP_LIBRARIES} ${JSON_GLIB_LIBRARIES} boost_iostreams)
