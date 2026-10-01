engine_add_module(renderer)

# volk: function pointers w/o boiler plate
find_package(volk CONFIG REQUIRED)
# VMA : vulkan memory manager
find_package(VulkanMemoryAllocator CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(Stb REQUIRED)
target_include_directories(engine_renderer PRIVATE ${Stb_INCLUDE_DIR})

target_link_libraries(engine_renderer
    PUBLIC volk::volk_headers
            mir::core
            # glm is PUBLIC: VulkanRenderer::DrawFrame takes glm::mat4 in the
            # public header. mir::core already carries glm publicly, but this
            # target's own interface needs it, so it is declared here too.
            glm::glm
    PRIVATE GPUOpen::VulkanMemoryAllocator
            imgui::imgui
            mir::serial
)

# compile volk ourself instead of linking prebuilt library
# due to platform definition issues
get_target_property(_volk_includes volk::volk_headers INTERFACE_INCLUDE_DIRECTORIES)
find_file(VOLK_SOURCE volk.c PATHS ${_volk_includes} NO_DEFAULT_PATH REQUIRED)
target_sources(engine_renderer PRIVATE ${VOLK_SOURCE})

# for volk macro definition
engine_target_vulkan_platform_defines(engine_renderer)

# makes VMA compatible with volk
target_compile_definitions(engine_renderer PRIVATE
    VMA_STATIC_VULKAN_FUNCTIONS=0
    VMA_DYNAMIC_VULKAN_FUNCTIONS=1
    VMA_VULKAN_VERSION=1004000
)

target_sources(engine_renderer PRIVATE
    src/renderer/CameraMath.cpp
    src/renderer/ComponentRegistration.cpp
    src/renderer/Renderer.cpp
    src/renderer/RenderSettings.cpp
    src/renderer/Shapes.cpp
    src/renderer/VulkanRenderer.cpp
    src/renderer/VulkanModel.cpp
    src/renderer/vulkan/VmaImpl.cpp
    src/renderer/vulkan/VulkanUtil.cpp
)
if(ENGINE_FAMILY_DESKTOP)
    if(ENGINE_PLATFORM STREQUAL "Windows")
    target_sources(engine_renderer PRIVATE src/renderer/vulkan/VulkanSurfaceWin32.cpp)
    elseif(ENGINE_PLATFORM STREQUAL "Linux")
    target_sources(engine_renderer PRIVATE src/renderer/vulkan/VulkanSurfaceLinux.cpp)
    else()
        message(FATAL_ERROR "No native window backend for ${ENGINE_PLATFORM}")
    endif()
endif()

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
