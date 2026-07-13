set(DYNAMIC_LOADER OFF CACHE BOOL "Build OpenXR loader as a static library" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_API_LAYERS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    openxr
    GIT_REPOSITORY https://github.com/KhronosGroup/OpenXR-SDK.git
    GIT_TAG        release-1.1.61
)

FetchContent_MakeAvailable(openxr)
