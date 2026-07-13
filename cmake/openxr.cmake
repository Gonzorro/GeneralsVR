set(DYNAMIC_LOADER OFF CACHE BOOL "Build OpenXR loader as a static library" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.61
)

FetchContent_MakeAvailable(openxr)

# GeneralsVR: Vulkan headers only (no loader - we call through DXVK's device and
# resolve entry points from vulkan-1.dll at runtime).
FetchContent_Declare(
    vulkan_headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        v1.4.309
)

FetchContent_MakeAvailable(vulkan_headers)
