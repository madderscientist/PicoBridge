# PICO 全身动捕桥接：NativeActivity + OpenXR(GLES) + WebSocket 上报

cmake_minimum_required(VERSION 3.22.1)
project("picobridge")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENXR_DIR ${CMAKE_CURRENT_SOURCE_DIR}/openxr)
set(JNI_LIBS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../jniLibs/${ANDROID_ABI})

# PICO 提供的预编译 loader，内含 XR_BD_body_tracking 等私有扩展的入口
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
        IMPORTED_LOCATION ${JNI_LIBS_DIR}/libopenxr_loader.so)

add_library(${CMAKE_PROJECT_NAME} SHARED
        main.cpp
        renderer.cpp
        ws_client.cpp
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
        ${ANDROID_NDK}/sources/android/native_app_glue
        ${OPENXR_DIR}/include)

target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
        XR_USE_PLATFORM_ANDROID
        XR_USE_GRAPHICS_API_OPENGL_ES)

target_link_libraries(${CMAKE_PROJECT_NAME}
        openxr_loader
        android
        log
        EGL
        GLESv3)

# glue 的入口没有被显式引用，不加这个会被链接器裁掉，导致 NativeActivity 起不来
target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "-u" "ANativeActivity_onCreate")
