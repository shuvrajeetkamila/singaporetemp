cmake_minimum_required(VERSION 3.22.1)
project(vkisland CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# NativeActivity glue is compiled straight into the library so its ANativeActivity_onCreate
# entry point is always exported (a static archive could be dropped by the linker).
add_library(vkisland SHARED
    main.cpp
    renderer.cpp
    world.cpp
    island.cpp
    diag.cpp
    camera.cpp
    geo.cpp
    hud.cpp
    diag_android.cpp
    sim.cpp
    agents.cpp
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
)

target_include_directories(vkisland PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ANDROID_NDK}/sources/android/native_app_glue
)

target_compile_definitions(vkisland PRIVATE VK_USE_PLATFORM_ANDROID_KHR=1)
target_compile_options(vkisland PRIVATE -Wall -Wextra -Wno-unused-parameter -fno-exceptions)

target_link_libraries(vkisland android log vulkan m)
