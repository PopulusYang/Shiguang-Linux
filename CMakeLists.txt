cmake_minimum_required(VERSION 3.20)
project(shiguang VERSION 0.1.0 LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)
pkg_check_modules(LIBCURL REQUIRED libcurl)

add_executable(shiguang
    src/main.c
    src/api.c
)

target_include_directories(shiguang PRIVATE
    ${GTK4_INCLUDE_DIRS}
    ${JSON_GLIB_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
)

target_link_directories(shiguang PRIVATE
    ${GTK4_LIBRARY_DIRS}
    ${JSON_GLIB_LIBRARY_DIRS}
    ${LIBCURL_LIBRARY_DIRS}
)

target_link_libraries(shiguang
    ${GTK4_LIBRARIES}
    ${JSON_GLIB_LIBRARIES}
    ${LIBCURL_LIBRARIES}
)

target_compile_options(shiguang PRIVATE
    ${GTK4_CFLAGS_OTHER}
    ${JSON_GLIB_CFLAGS_OTHER}
    ${LIBCURL_CFLAGS_OTHER}
)
