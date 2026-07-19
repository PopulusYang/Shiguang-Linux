cmake_minimum_required(VERSION 3.20)
project(shiguang VERSION 0.1.1 LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED gtk4)
pkg_check_modules(GLIB REQUIRED glib-2.0)
pkg_check_modules(JSON_GLIB REQUIRED json-glib-1.0)
pkg_check_modules(LIBCURL REQUIRED libcurl)

# GResource 图标编译
set(GRESOURCE_XML ${CMAKE_SOURCE_DIR}/icon.gresource.xml)
set(GRESOURCE_C   ${CMAKE_BINARY_DIR}/icon.gresource.c)
add_custom_command(
    OUTPUT ${GRESOURCE_C}
    COMMAND glib-compile-resources
        --target=${GRESOURCE_C}
        --sourcedir=${CMAKE_SOURCE_DIR}
        --generate-source
        ${GRESOURCE_XML}
    DEPENDS ${GRESOURCE_XML} ${CMAKE_SOURCE_DIR}/icon.png
)

add_executable(shiguang
    src/main.c
    src/api.c
    ${GRESOURCE_C}
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

# ---- 命令行压力测试（模拟多线程队列操作，需要 GTK 主循环）----
add_executable(stress_test
    tests/stress_test.c
    src/api.c
)

target_include_directories(stress_test PRIVATE
    ${GLIB_INCLUDE_DIRS}
    ${JSON_GLIB_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
)

target_link_directories(stress_test PRIVATE
    ${GLIB_LIBRARY_DIRS}
    ${JSON_GLIB_LIBRARY_DIRS}
    ${LIBCURL_LIBRARY_DIRS}
)

target_link_libraries(stress_test
    ${GLIB_LIBRARIES}
    ${JSON_GLIB_LIBRARIES}
    ${LIBCURL_LIBRARIES}
)

target_compile_options(stress_test PRIVATE
    ${JSON_GLIB_CFLAGS_OTHER}
    ${LIBCURL_CFLAGS_OTHER}
)
add_executable(test_api
    tests/test_api.c
    src/api.c
)

target_include_directories(test_api PRIVATE
    ${JSON_GLIB_INCLUDE_DIRS}
    ${LIBCURL_INCLUDE_DIRS}
)

target_link_directories(test_api PRIVATE
    ${JSON_GLIB_LIBRARY_DIRS}
    ${LIBCURL_LIBRARY_DIRS}
)

target_link_libraries(test_api
    ${JSON_GLIB_LIBRARIES}
    ${LIBCURL_LIBRARIES}
)

target_compile_options(test_api PRIVATE
    ${JSON_GLIB_CFLAGS_OTHER}
    ${LIBCURL_CFLAGS_OTHER}
)

# 安装规则
install(TARGETS shiguang RUNTIME DESTINATION bin)
install(FILES io.github.populusyang.shiguang.desktop
        DESTINATION share/applications)
install(FILES icon.png
        DESTINATION share/icons/hicolor/256x256/apps
        RENAME io.github.populusyang.shiguang.png)
