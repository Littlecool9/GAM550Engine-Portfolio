/**
 * @file VulkanRenderer.cpp
 * @author Sumin Park
 * @brief Renderer with all Vulkan signatures.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "renderer/VulkanRenderer.h"
#include "vulkan/VulkanSurface.h"
#include <core/log/Log.h>
#include <core/log/Assert.h>
#include <core/fs/Paths.h>
#include <core/fs/FileIO.h>

#include <vk_mem_alloc.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include <cstring>
#include <algorithm>
#include <cmath>
#include <optional>
#include <array>
#include <chrono>
#include <string_view>
#include <format>

namespace mir
{
    namespace
    {
        bool CreateBuffer(VmaAllocator allocator,
            VkDeviceSize size,
            VkBufferUsageFlags usage,
            VmaAllocationCreateFlags allocFlags,
            VkBuffer &outBuffer,
            VmaAllocation &outAllocation,
            VmaAllocationInfo *outInfo)
        {
            const VkBufferCreateInfo bufferInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size = size,
                .usage = usage,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE };

            const VmaAllocationCreateInfo allocInfo{
                .flags = allocFlags,
                .usage = VMA_MEMORY_USAGE_AUTO };

            return vmaCreateBuffer(allocator, &bufferInfo, &allocInfo,
                &outBuffer, &outAllocation, outInfo) == VK_SUCCESS;
        }
        // normalCols = Normal matrix
        // vulkan's guaranteed minimum constant size = 128
        // vulkan pads every column to 16 anyways
        struct PushData
        {
            glm::mat4 model;      // 64
            glm::vec4 normalCol0; // 16
            glm::vec4 normalCol1; // 16
            glm::vec4 normalCol2; // 16
            glm::vec4 tint;       // 16
        };

        constexpr std::string_view kModelShaderName = "model";
        struct CameraData
        {
            glm::mat4 viewProj;
            glm::vec4 lightDirWS;   // xyz = normalized direction *toward* the light/sun
            glm::vec4 lightColor;   // rgb = light color/intensity
            glm::vec4 ambientColor; // rgb = flat ambient term
            glm::mat4 invViewProj;
            glm::vec4 cameraPosWS;  // xyz = camera world position
        };

        VkDeviceSize AlignUp(VkDeviceSize size, VkDeviceSize alignment)
        {
            if (alignment == 0)
                return size;
            return (size + alignment - 1) & ~(alignment - 1);
        }
        VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
            VkDebugUtilsMessageSeverityFlagBitsEXT severity,
            VkDebugUtilsMessageTypeFlagsEXT,
            const VkDebugUtilsMessengerCallbackDataEXT *data,
            void *)
        {
            if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            {
                MIR_LOG_ERROR("[vulkan] {}", data->pMessage);
                MIR_DEBUG_BREAK();
            }
            else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
            {
                MIR_LOG_WARN("[vulkan] {}", data->pMessage);
            }
            else
            {
                MIR_LOG_INFO("[vulkan] {}", data->pMessage);
            }

            // VK_TRUE would abort the call that triggered this. That is a layer
            // self-test mechanism, not an error handler. Always VK_FALSE.
            return VK_FALSE;
        }

        VkDebugUtilsMessengerCreateInfoEXT MakeMessengerCreateInfo()
        {
            return VkDebugUtilsMessengerCreateInfoEXT{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = &DebugCallback };
        }

        bool HasLayer(const std::vector<VkLayerProperties> &layers, const char *name)
        {
            for (const VkLayerProperties &layer : layers)
            {
                if (std::strcmp(layer.layerName, name) == 0)
                    return true;
            }
            return false;
        }

        bool HasExtension(const std::vector<VkExtensionProperties> &exts, const char *name)
        {
            for (const VkExtensionProperties &ext : exts)
            {
                if (std::strcmp(ext.extensionName, name) == 0)
                    return true;
            }
            return false;
        }

        bool HasDeviceExtension(VkPhysicalDevice device, const char *name)
        {
            uint32_t count = 0;
            vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
            std::vector<VkExtensionProperties> exts(count);
            vkEnumerateDeviceExtensionProperties(device, nullptr, &count, exts.data());

            for (const VkExtensionProperties &ext : exts)
            {
                if (std::strcmp(ext.extensionName, name) == 0)
                    return true;
            }
            return false;
        }

        bool HasRequiredFeatures(VkPhysicalDevice device)
        {
            VkPhysicalDeviceVulkan13Features features13{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };

            VkPhysicalDeviceVulkan12Features features12{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                .pNext = &features13 };

            VkPhysicalDeviceVulkan11Features features11{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                .pNext = &features12 };

            VkPhysicalDeviceFeatures2 features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &features11 };

            vkGetPhysicalDeviceFeatures2(device, &features);

            return features13.dynamicRendering == VK_TRUE &&
                features13.synchronization2 == VK_TRUE &&
                features12.timelineSemaphore == VK_TRUE &&
                features11.shaderDrawParameters == VK_TRUE;
        }
        bool HasSurfaceSupport(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            uint32_t formatCount = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

            uint32_t presentModeCount = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);

            return formatCount > 0 && presentModeCount > 0;
        }

        uint32_t FindGraphicsPresentFamily(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
            std::vector<VkQueueFamilyProperties> families(count);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &count, families.data());

            for (uint32_t i = 0; i < count; ++i)
            {
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0)
                    continue;

                VkBool32 presentSupported = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupported);

                if (presentSupported == VK_TRUE)
                    return i;
            }
            return UINT32_MAX;
        }

        VkSurfaceFormatKHR ChooseSurfaceFormat(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, nullptr);
            std::vector<VkSurfaceFormatKHR> formats(count);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &count, formats.data());

            for (const VkSurfaceFormatKHR &format : formats)
            {
                if (format.format == VK_FORMAT_B8G8R8A8_SRGB &&
                    format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
                    return format;
            }

            // fall back to basic format. It'll have the basics
            return formats[0];
        }

        VkPresentModeKHR ChoosePresentMode(VkPhysicalDevice device, VkSurfaceKHR surface)
        {
            uint32_t count = 0;
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, nullptr);
            std::vector<VkPresentModeKHR> modes(count);
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &count, modes.data());

            for (VkPresentModeKHR mode : modes)
            {
                if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
                    return mode;
            }

            // FIFO is the only mode the spec guarantees exists.
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        VkExtent2D ChooseExtent(const VkSurfaceCapabilitiesKHR &caps, const ISurfaceProvider &window)
        {
            if (caps.currentExtent.width != UINT32_MAX)
                return caps.currentExtent;

            // Framebuffer size, not window size: they differ on HiDPI.
            VkExtent2D extent{ window.Width(), window.Height() };

            extent.width = std::clamp(extent.width,
                caps.minImageExtent.width,
                caps.maxImageExtent.width);
            extent.height = std::clamp(extent.height,
                caps.minImageExtent.height,
                caps.maxImageExtent.height);
            return extent;
        }
        void ImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageAspectFlags aspectMask,
            VkImageLayout oldLayout, VkImageLayout newLayout,
            VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
            uint32_t levelCount = 1)
        {
            const VkImageMemoryBarrier2 barrier{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                .srcStageMask = srcStage,
                .srcAccessMask = srcAccess,
                .dstStageMask = dstStage,
                .dstAccessMask = dstAccess,
                .oldLayout = oldLayout,
                .newLayout = newLayout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange{
                    .aspectMask = aspectMask,
                    .levelCount = levelCount,
                    .layerCount = 1 } };

            const VkDependencyInfo dep{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &barrier };

            vkCmdPipelineBarrier2(cmd, &dep);
        }
        // Level 0, then every smaller level if desc asks for them, packed back
        // to back into one staging blob with a copy region each. Returns the
        // level count. The box filter averages in linear light, so an sRGB
        // image is decoded and re-encoded around the average; alpha is already
        // linear and is averaged as is.
        uint32_t BuildMipChain(const TextureDesc &desc, std::vector<std::byte> &bytes,
            std::vector<VkBufferImageCopy> &regions)
        {
            const auto toLinear = [](float v)
                { return v <= 0.04045f ? v / 12.92f : std::pow((v + 0.055f) / 1.055f, 2.4f); };
            const auto toSrgb = [](float v)
                { return v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f; };

            bytes.assign(desc.pixels.begin(), desc.pixels.end());
            std::vector<std::byte> level(desc.pixels.begin(), desc.pixels.end());

            uint32_t width = desc.width;
            uint32_t height = desc.height;
            uint32_t mip = 0;
            VkDeviceSize offset = 0;

            for (;;)
            {
                regions.push_back(VkBufferImageCopy{
                    .bufferOffset = offset,
                    .imageSubresource{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .mipLevel = mip,
                        .baseArrayLayer = 0,
                        .layerCount = 1 },
                    .imageExtent{ width, height, 1 } });

                if (!desc.generateMips || (width == 1 && height == 1))
                    break;

                const uint32_t nextWidth = std::max(width / 2, 1u);
                const uint32_t nextHeight = std::max(height / 2, 1u);
                std::vector<std::byte> next(static_cast<size_t>(nextWidth) * nextHeight * 4);

                for (uint32_t y = 0; y < nextHeight; ++y)
                    for (uint32_t x = 0; x < nextWidth; ++x)
                        for (uint32_t c = 0; c < 4; ++c)
                        {
                            float sum = 0.0f;
                            uint32_t samples = 0;
                            for (uint32_t sy = y * height / nextHeight; sy < (y + 1) * height / nextHeight; ++sy)
                                for (uint32_t sx = x * width / nextWidth; sx < (x + 1) * width / nextWidth; ++sx)
                                {
                                    const float v =
                                        std::to_integer<uint8_t>(level[(static_cast<size_t>(sy) * width + sx) * 4 + c]) / 255.0f;
                                    sum += (c < 3 && desc.srgb) ? toLinear(v) : v;
                                    ++samples;
                                }

                            float v = sum / static_cast<float>(samples);
                            if (c < 3 && desc.srgb)
                                v = toSrgb(v);
                            next[(static_cast<size_t>(y) * nextWidth + x) * 4 + c] =
                                static_cast<std::byte>(std::clamp<long>(std::lround(v * 255.0f), 0, 255));
                        }

                offset = bytes.size();
                bytes.insert(bytes.end(), next.begin(), next.end());
                level = std::move(next);
                width = nextWidth;
                height = nextHeight;
                ++mip;
            }

            return mip + 1;
        }
        std::optional<std::vector<uint32_t>> ReadSpirv(const std::filesystem::path &path)
        {
            const std::optional<std::vector<std::byte>> bytes = ReadBytes(path);
            if (!bytes)
                // ReadBytes takes care of logging onfailure
                return std::nullopt;

            if (bytes->empty() || bytes->size() % sizeof(uint32_t) != 0)
            {
                MIR_LOG_CRITICAL("Bad SPIR-V size ({} bytes): {}", bytes->size(), path.string());
                return std::nullopt;
            }

            // Copied, not reinterpreted: vkCreateShaderModule needs pCode 4-byte
            // aligned and a vector<byte> is only aligned to 1.
            std::vector<uint32_t> words(bytes->size() / sizeof(uint32_t));
            std::memcpy(words.data(), bytes->data(), bytes->size());
            return words;
        }

        VkShaderModule CreateShaderModule(VkDevice device, const std::filesystem::path &path)
        {
            const std::optional<std::vector<uint32_t>> words = ReadSpirv(path);
            if (!words.has_value())
                return VK_NULL_HANDLE;

            const VkShaderModuleCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = words->size() * sizeof(uint32_t),
                .pCode = words->data() };

            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vkCreateShaderModule failed: {}", path.string());
                return VK_NULL_HANDLE;
            }
            return module;
        }
    }

    bool VulkanRenderer::Initialize(const RendererDesc &desc)
    {
        if (desc.window == nullptr)
        {
            MIR_LOG_ERROR("no window");
            return false;
        }

        mWindow = desc.window;

        if (volkInitialize() != VK_SUCCESS)
        {
            MIR_LOG_ERROR("volk initialized failed");
            return false;
        }

        if (!CreateVulkanInstance(desc))
            return false;

        volkLoadInstance(mVulkanInstance);

        if (mValidationEnabled && !CreateDebugMessenger())
            return false;

        if (!CreateSurface())
            return false;

        if (!FindPhysicalDevice())
            return false;

        if (!CreateLogicalDevice())
            return false;

        if (!CreateAllocator())
            return false;

        if (!CreateSwapchain())
            return false;

        if (!CreateFrameResources())
            return false;

        if (!CreateDefaultTextures())
            return false;

        if (!CreateCameraResources())
            return false;

        if (!CreatePipelineLayout())
            return false;

        if (!CreateSkyPipeline())
            return false;

        mDefaultMaterial = CreateMaterial(MaterialDesc{});
        if (mDefaultMaterial.IsNull())
            return false;

        NameObject(VK_OBJECT_TYPE_DEVICE, reinterpret_cast<uint64_t>(mDevice), "MjolnirEngine device");
        NameObject(VK_OBJECT_TYPE_QUEUE, reinterpret_cast<uint64_t>(mGfxQueue), "Graphics+present queue");
        NameObject(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<uint64_t>(mSwapchain), "Swapchain");
        NameObject(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(mMaterials[mDefaultMaterial.mIndex].mPipeline), "Default material pipeline");
        NameObject(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(mSkyPipeline), "Sky pipeline");

        for (size_t i = 0; i < mSwapchainImages.size(); ++i)
        {
            NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mSwapchainImages[i]),
                std::format("Swapchain image {}", i).c_str());
        }

        return true;
    }

    namespace
    {
        void CheckImGuiVulkanResult(VkResult result)
        {
            if (result != VK_SUCCESS)
                MIR_LOG_ERROR("ImGui Vulkan backend call failed: {}", static_cast<int>(result));
        }
    }

    bool VulkanRenderer::InitImGuiVulkanBackend()
    {
        VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &mSwapchainFormat,
            .depthAttachmentFormat = kDepthFormat };

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = VulkanVersion;
        initInfo.Instance = mVulkanInstance;
        initInfo.PhysicalDevice = mPhysicalDevice;
        initInfo.Device = mDevice;
        initInfo.QueueFamily = mGfxQueueFamIdx;
        initInfo.Queue = mGfxQueue;
        initInfo.DescriptorPoolSize = 64;
        initInfo.MinImageCount = static_cast<uint32_t>(mSwapchainImages.size());
        initInfo.ImageCount = static_cast<uint32_t>(mSwapchainImages.size());
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo = renderingInfo;
        initInfo.CheckVkResultFn = &CheckImGuiVulkanResult;

        if (!ImGui_ImplVulkan_Init(&initInfo))
        {
            MIR_LOG_ERROR("ImGui_ImplVulkan_Init failed");
            return false;
        }

        mImGuiBackendInitialized = true;
        return true;
    }

    void VulkanRenderer::ShutdownImGuiVulkanBackend()
    {
        if (!mImGuiBackendInitialized)
            return;

        vkDeviceWaitIdle(mDevice);
        ImGui_ImplVulkan_Shutdown();
        mImGuiBackendInitialized = false;
    }

    void VulkanRenderer::Shutdown()
    {
        if (mDevice != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(mDevice);

            if (mTimeline != VK_NULL_HANDLE)
            {
                vkDestroySemaphore(mDevice, mTimeline, nullptr);
                mTimeline = VK_NULL_HANDLE;
            }

            for (uint32_t i = 0; i < kFramesInFlight; ++i)
            {
                if (mImageAcquired[i] != VK_NULL_HANDLE)
                    vkDestroySemaphore(mDevice, mImageAcquired[i], nullptr);
                // Destroying the pool frees its command buffers too.
                if (mCmdPools[i] != VK_NULL_HANDLE)
                    vkDestroyCommandPool(mDevice, mCmdPools[i], nullptr);
            }

            // before mAllocator
            DestroyMeshes();
            // before mMaterialDescriptorPool: frees each material's set
            DestroyMaterials();
            DestroyModelResources(true);
            DestroyTextures();
            // before mAllocator: frees the camera UBO buffers
            DestroyCameraResources();

            DestroySamplers();

            if (mMaterialDescriptorPool != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorPool(mDevice, mMaterialDescriptorPool, nullptr);
                mMaterialDescriptorPool = VK_NULL_HANDLE;
            }

            if (mMaterialSetLayout != VK_NULL_HANDLE)
            {
                vkDestroyDescriptorSetLayout(mDevice, mMaterialSetLayout, nullptr);
                mMaterialSetLayout = VK_NULL_HANDLE;
            }

            if (mPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(mDevice, mPipelineLayout, nullptr);
                mPipelineLayout = VK_NULL_HANDLE;
            }

            if (mSkyPipeline != VK_NULL_HANDLE)
            {
                vkDestroyPipeline(mDevice, mSkyPipeline, nullptr);
                mSkyPipeline = VK_NULL_HANDLE;
            }

            if (mSkyPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(mDevice, mSkyPipelineLayout, nullptr);
                mSkyPipelineLayout = VK_NULL_HANDLE;
            }

            DestroyRenderCompleteSemaphores();
            DestroySwapchain();
        }

        if (mAllocator != VK_NULL_HANDLE)
        {
            // The permanent leak detector. Every later milestone that allocates
            // must still land here at 0 bytes.
            VmaTotalStatistics stats{};
            vmaCalculateStatistics(mAllocator, &stats);
            MIR_LOG_INFO("[vma] live bytes at shutdown: {} in {} allocation(s)",
                stats.total.statistics.allocationBytes,
                stats.total.statistics.allocationCount);

            vmaDestroyAllocator(mAllocator);
            mAllocator = VK_NULL_HANDLE;
        }

        if (mDevice != VK_NULL_HANDLE)
        {
            vkDestroyDevice(mDevice, nullptr);
            mDevice = VK_NULL_HANDLE;
        }

        if (mDebugMessenger != VK_NULL_HANDLE)
        {
            vkDestroyDebugUtilsMessengerEXT(mVulkanInstance, mDebugMessenger, nullptr);
            mDebugMessenger = VK_NULL_HANDLE;
        }

        if (mSurface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(mVulkanInstance, mSurface, nullptr);
            mSurface = VK_NULL_HANDLE;
        }

        if (mVulkanInstance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(mVulkanInstance, nullptr);
            mVulkanInstance = VK_NULL_HANDLE;
        }

        volkFinalize();
    }
    bool VulkanRenderer::CreateVulkanInstance(const RendererDesc &desc)
    {
        // to fill data, read twice : once for count, once to fill
        uint32_t layerCount = 0;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        uint32_t extCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> availableExts(extCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, availableExts.data());

        std::vector<const char *> layers;
        std::vector<const char *> extensions;

        const WindowBackend backend = mWindow->NativeWindow().backend;

        const char *platformExt = vk::PlatformSurfaceExtension(backend);
        if (platformExt == nullptr)
        {
            MIR_LOG_CRITICAL("No Vulkan surface extension for window backend {}",
                static_cast<int>(backend));
            return false;
        }

        extensions.push_back(VK_KHR_SURFACE_EXTENSION_NAME);
        extensions.push_back(platformExt);

        for (const char *name : extensions)
        {
            if (!HasExtension(availableExts, name))
            {
                MIR_LOG_CRITICAL("Required instance extension missing: {}", name);
                return false;
            }
        }

        // debug soft check
        if (desc.enableValidation)
        {
            const bool hasLayer = HasLayer(availableLayers, "VK_LAYER_KHRONOS_validation");
            const bool hasDebugUtils = HasExtension(availableExts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

            if (hasLayer && hasDebugUtils)
            {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
                mValidationEnabled = true;
            }
            else
            {
                MIR_LOG_WARN("Validation unavailable (layer: {}, debug_utils: {}); "
                    "install the Vulkan SDK to enable it",
                    hasLayer, hasDebugUtils);
            }
        }

        VkApplicationInfo appInfo{
            .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
            .pApplicationName = desc.appName,
            .apiVersion = VulkanVersion };

        // temporary for instance creation.
        VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MakeMessengerCreateInfo();

        // Finds hazards the ordinary validation layers miss
        const VkValidationFeatureEnableEXT syncValidationFeature =
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;

        const VkValidationFeaturesEXT validationFeatures{
            .sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
            .pNext = &messengerInfo,
            .enabledValidationFeatureCount = 1,
            .pEnabledValidationFeatures = &syncValidationFeature };

        VkInstanceCreateInfo createInfo{
            .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext = mValidationEnabled ? static_cast<const void *>(&validationFeatures) : nullptr,
            .pApplicationInfo = &appInfo,
            .enabledLayerCount = static_cast<uint32_t>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<uint32_t>(extensions.size()),
            .ppEnabledExtensionNames = extensions.data() };

        if (vkCreateInstance(&createInfo, nullptr, &mVulkanInstance) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateInstance failed");
            return false;
        }

        return true;
    }
    bool VulkanRenderer::CreateDebugMessenger()
    {
        const VkDebugUtilsMessengerCreateInfoEXT info = MakeMessengerCreateInfo();

        if (vkCreateDebugUtilsMessengerEXT(mVulkanInstance, &info, nullptr, &mDebugMessenger) != VK_SUCCESS)
        {
            MIR_LOG_ERROR("vkCreateDebugUtilsMessengerEXT failed");
            return false;
        }
        return true;
    }
    bool VulkanRenderer::CreateSurface()
    {
        mSurface = vk::CreateSurface(mVulkanInstance, mWindow->NativeWindow());

        if (mSurface == VK_NULL_HANDLE)
        {
            MIR_LOG_CRITICAL("Vulkan surface creation failed");
            return false;
        }
        return true;
    }

    bool VulkanRenderer::FindPhysicalDevice()
    {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(mVulkanInstance, &count, nullptr);
        if (count == 0)
        {
            MIR_LOG_CRITICAL("No Vulkan-capable GPU found");
            return false;
        }

        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(mVulkanInstance, &count, devices.data());

        uint32_t bestScore = 0;

        for (VkPhysicalDevice device : devices)
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(device, &props);

            // Every rejection says which device and why
            if (props.apiVersion < VulkanVersion)
            {
                MIR_LOG_INFO("Rejected {}: Vulkan {}.{}, need 1.4", props.deviceName,
                    VK_API_VERSION_MAJOR(props.apiVersion),
                    VK_API_VERSION_MINOR(props.apiVersion));
                continue;
            }
            if (!HasDeviceExtension(device, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
            {
                MIR_LOG_INFO("Rejected {}: no swapchain extension", props.deviceName);
                continue;
            }
            if (!HasRequiredFeatures(device))
            {
                MIR_LOG_INFO("Rejected {}: missing dynamicRendering / sync2 / timeline / shaderDrawParameters", props.deviceName);
                continue;
            }
            if (!HasSurfaceSupport(device, mSurface))
            {
                MIR_LOG_INFO("Rejected {}: no surface formats or present modes", props.deviceName);
                continue;
            }

            const uint32_t family = FindGraphicsPresentFamily(device, mSurface);
            if (family == UINT32_MAX)
            {
                MIR_LOG_INFO("Rejected {}: no graphics+present queue family", props.deviceName);
                continue;
            }

            const uint32_t score =
                (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) ? 1000 : 100;

            if (score > bestScore)
            {
                bestScore = score;
                mPhysicalDevice = device;
                mGfxQueueFamIdx = family;
            }
        }

        if (mPhysicalDevice == VK_NULL_HANDLE)
        {
            MIR_LOG_CRITICAL("No suitable GPU among {} candidate(s)", count);
            return false;
        }

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(mPhysicalDevice, &props);
        MIR_LOG_INFO("GPU: {} | {} | Vulkan {}.{}.{} | graphics+present family {}",
            props.deviceName,
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" : "integrated",
            VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion),
            mGfxQueueFamIdx);
        return true;
    }

    bool VulkanRenderer::CreateLogicalDevice()
    {
        // required even for single queue, needed in infostruct
        const float queuePriority = 1.0f;

        const VkDeviceQueueCreateInfo queueInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = mGfxQueueFamIdx,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority };

        // list of extensions at 1.4
        // dynamic rendering, sync resources are core, not extensions
        // .pNext to link multiple extensions together
        const char *deviceExtensions[]{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };

        VkPhysicalDeviceVulkan13Features features13{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
            .synchronization2 = VK_TRUE,
            .dynamicRendering = VK_TRUE };

        VkPhysicalDeviceVulkan12Features features12{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
            .pNext = &features13,
            .descriptorIndexing = VK_TRUE,
            .timelineSemaphore = VK_TRUE,
            .bufferDeviceAddress = VK_TRUE };

        VkPhysicalDeviceVulkan11Features features11{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
            .pNext = &features12,
            .shaderDrawParameters = VK_TRUE };

        VkPhysicalDeviceFeatures2 features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = &features11 };

        const VkDeviceCreateInfo deviceInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &features,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueInfo,
            .enabledExtensionCount = 1,
            .ppEnabledExtensionNames = deviceExtensions,
            .pEnabledFeatures = nullptr // must be nullptr, everything goes through .pNext
        };

        if (vkCreateDevice(mPhysicalDevice, &deviceInfo, nullptr, &mDevice) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateDevice Failed");
            return false;
        }

        volkLoadDevice(mDevice);
        vkGetDeviceQueue(mDevice, mGfxQueueFamIdx, 0, &mGfxQueue);

        return true;
    }
    bool VulkanRenderer::CreateAllocator()
    {
        const VmaVulkanFunctions functions{
            .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = vkGetDeviceProcAddr };

        const VmaAllocatorCreateInfo allocatorInfo{
            .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
            .physicalDevice = mPhysicalDevice,
            .device = mDevice,
            .pVulkanFunctions = &functions,
            .instance = mVulkanInstance,
            .vulkanApiVersion = VulkanVersion };

        if (vmaCreateAllocator(&allocatorInfo, &mAllocator) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vmaCreateAllocator failed");
            return false;
        }
        return true;
    }
    bool VulkanRenderer::CreateSwapchain()
    {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(mPhysicalDevice, mSurface, &caps);

        const VkSurfaceFormatKHR surfaceFormat = ChooseSurfaceFormat(mPhysicalDevice, mSurface);
        const VkPresentModeKHR presentMode = ChoosePresentMode(mPhysicalDevice, mSurface);
        const VkExtent2D extent = ChooseExtent(caps, *mWindow);
        mCanCapture = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;

        uint32_t imageCount = caps.minImageCount + 1;
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount)
            imageCount = caps.maxImageCount;

        VkSwapchainKHR oldSwapchain = mSwapchain;

        const VkSwapchainCreateInfoKHR info{
            .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = mSurface,
            .minImageCount = imageCount,
            .imageFormat = surfaceFormat.format,
            .imageColorSpace = surfaceFormat.colorSpace,
            .imageExtent = extent,
            .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | (mCanCapture ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0u),
            // One family does graphics and present, so no ownership transfers.
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform = caps.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode = presentMode,
            // Allow the driver to skip shading pixels hidden by other windows.
            .clipped = VK_TRUE,
            // reuse old resources
            .oldSwapchain = oldSwapchain };

        if (vkCreateSwapchainKHR(mDevice, &info, nullptr, &mSwapchain) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateSwapchainKHR failed");
            mSwapchain = VK_NULL_HANDLE;
            return false;
        }

        // Retired, not owned by the new swapchain: destroy it ourselves.
        if (oldSwapchain != VK_NULL_HANDLE)
            vkDestroySwapchainKHR(mDevice, oldSwapchain, nullptr);

        mSwapchainFormat = surfaceFormat.format;
        mSwapchainExtent = extent;

        // The swapchain creates and owns these. We never allocate or free them.
        uint32_t actualCount = 0;
        vkGetSwapchainImagesKHR(mDevice, mSwapchain, &actualCount, nullptr);
        mSwapchainImages.resize(actualCount);
        vkGetSwapchainImagesKHR(mDevice, mSwapchain, &actualCount, mSwapchainImages.data());

        MIR_LOG_INFO("Swapchain: {}x{} | {} image(s), requested {} | present mode {}",
            extent.width, extent.height, actualCount, imageCount,
            presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO");

        return CreateImageViews() && CreateDepthResources();
    }
    bool VulkanRenderer::CreateImageViews()
    {
        mSwapchainViews.resize(mSwapchainImages.size(), VK_NULL_HANDLE);

        for (size_t i = 0; i < mSwapchainImages.size(); ++i)
        {
            const VkImageViewCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mSwapchainImages[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = mSwapchainFormat,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 } };

            if (vkCreateImageView(mDevice, &info, nullptr, &mSwapchainViews[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vkCreateImageView failed for swapchain image {}", i);
                return false;
            }
        }

        return true;
    }
    bool VulkanRenderer::CreateDepthResources()
    {
        VkFormatProperties formatProps{};
        vkGetPhysicalDeviceFormatProperties(mPhysicalDevice, kDepthFormat, &formatProps);
        if ((formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0)
        {
            MIR_LOG_CRITICAL("VK_FORMAT_D32_SFLOAT unsupported as an optimal-tiling depth attachment");
            return false;
        }

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            const VkImageCreateInfo imageInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = kDepthFormat,
                .extent{ mSwapchainExtent.width, mSwapchainExtent.height, 1 },
                .mipLevels = 1,
                .arrayLayers = 1,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

            const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

            if (vmaCreateImage(mAllocator, &imageInfo, &allocInfo,
                &mDepthImages[i], &mDepthAllocations[i], nullptr) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vmaCreateImage failed for depth buffer {}", i);
                return false;
            }

            const VkImageViewCreateInfo viewInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = mDepthImages[i],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = kDepthFormat,
                .subresourceRange{
                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = 1 } };

            if (vkCreateImageView(mDevice, &viewInfo, nullptr, &mDepthViews[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vkCreateImageView failed for depth buffer {}", i);
                return false;
            }

            NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mDepthImages[i]),
                std::format("Depth buffer {}", i).c_str());
        }

        return true;
    }

    void VulkanRenderer::DestroyDepthResources()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mDepthViews[i] != VK_NULL_HANDLE)
            {
                vkDestroyImageView(mDevice, mDepthViews[i], nullptr);
                mDepthViews[i] = VK_NULL_HANDLE;
            }
            if (mDepthImages[i] != VK_NULL_HANDLE)
            {
                vmaDestroyImage(mAllocator, mDepthImages[i], mDepthAllocations[i]);
                mDepthImages[i] = VK_NULL_HANDLE;
                mDepthAllocations[i] = VK_NULL_HANDLE;
            }
        }
    }

    void VulkanRenderer::DestroySwapchain()
    {
        // swapchain and depth go hand in hand
        DestroyDepthResources();

        for (VkImageView view : mSwapchainViews)
        {
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(mDevice, view, nullptr);
        }
        mSwapchainViews.clear();
        mSwapchainImages.clear();

        if (mSwapchain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(mDevice, mSwapchain, nullptr);
            mSwapchain = VK_NULL_HANDLE;
        }
    }

    bool VulkanRenderer::RecreateSwapchain()
    {
        vkDeviceWaitIdle(mDevice);
        DestroyRenderCompleteSemaphores();
        DestroySwapchain();

        if (!CreateSwapchain())
            return false;
        if (!CreateRenderCompleteSemaphores())
            return false;

        mNeedRecreate = false;
        return true;
    }

    bool VulkanRenderer::CreatePipelineLayout()
    {
        // set 2 first: CreatePipelineLayout puts its layout in the pipeline.
        if (!CreateModelDescriptors())
            return false;

        // binding 0 = base color, binding 1 = normal map. Every material
        // uses this same layout, whether or not its shader samples them.
        const std::array<VkDescriptorSetLayoutBinding, 2> setBindings{ {
            { .binding = 0,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .descriptorCount = 1,
             .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT } } };

        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = static_cast<uint32_t>(setBindings.size()),
            .pBindings = setBindings.data() };

        if (vkCreateDescriptorSetLayout(mDevice, &setLayoutInfo, nullptr, &mMaterialSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateDescriptorSetLayout failed");
            return false;
        }

        // One set per material. A model primitive eats two (front and mirrored
        // front face), so this is not as generous as it looks.
        constexpr uint32_t kMaxMaterials = 1024;
        const VkDescriptorPoolSize poolSize{
            .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .descriptorCount = kMaxMaterials * static_cast<uint32_t>(setBindings.size()) };

        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
            .maxSets = kMaxMaterials,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize };

        if (vkCreateDescriptorPool(mDevice, &poolInfo, nullptr, &mMaterialDescriptorPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateDescriptorPool failed");
            return false;
        }

        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = sizeof(PushData) };

        // set 0 = material (textures), set 1 = per-view camera UBO,
        // set 2 = per-draw model data.
        const std::array<VkDescriptorSetLayout, 3> setLayouts{
            mMaterialSetLayout, mCameraSetLayout, mModelSetLayout };

        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = static_cast<uint32_t>(setLayouts.size()),
            .pSetLayouts = setLayouts.data(),
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange };

        if (vkCreatePipelineLayout(mDevice, &layoutInfo, nullptr, &mPipelineLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreatePipelineLayout failed");
            return false;
        }

        return true;
    }

    bool VulkanRenderer::CreateSkyPipeline()
    {
        VkShaderModule vertModule = CreateShaderModule(mDevice, ShaderPath("sky.vertexMain.spv"));
        VkShaderModule fragModule = CreateShaderModule(mDevice, ShaderPath("sky.fragmentMain.spv"));

        if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE)
        {
            if (vertModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice, vertModule, nullptr);
            if (fragModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice, fragModule, nullptr);
            return false;
        }

        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &mCameraSetLayout };

        if (vkCreatePipelineLayout(mDevice, &layoutInfo, nullptr, &mSkyPipelineLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateSkyPipeline: vkCreatePipelineLayout failed");
            vkDestroyShaderModule(mDevice, vertModule, nullptr);
            vkDestroyShaderModule(mDevice, fragModule, nullptr);
            return false;
        }

        const VkPipelineShaderStageCreateInfo stages[]{
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT,
             .module = vertModule,
             .pName = "vertexMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = fragModule,
             .pName = "fragmentMain" } };

        const VkPipelineVertexInputStateCreateInfo vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };

        const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE };

        const VkPipelineViewportStateCreateInfo viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = nullptr,
            .scissorCount = 1,
            .pScissors = nullptr };

        const VkPipelineRasterizationStateCreateInfo rasterizer{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .depthBiasEnable = VK_FALSE,
            .lineWidth = 1.0f };

        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE };

        const VkPipelineColorBlendAttachmentState blendAttachment{
            .blendEnable = VK_FALSE,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };

        const VkPipelineDepthStencilStateCreateInfo depthStencil{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = VK_FALSE,
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE };

        const VkPipelineColorBlendStateCreateInfo colorBlend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = VK_FALSE,
            .attachmentCount = 1,
            .pAttachments = &blendAttachment };

        const VkDynamicState dynamicStates[]{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR };

        const VkPipelineDynamicStateCreateInfo dynamicState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamicStates };

        const VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &mSwapchainFormat,
            .depthAttachmentFormat = kDepthFormat };

        const VkGraphicsPipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &renderingInfo,
            .stageCount = 2,
            .pStages = stages,
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterizer,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depthStencil,
            .pColorBlendState = &colorBlend,
            .pDynamicState = &dynamicState,
            .layout = mSkyPipelineLayout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0 };

        const VkResult result = vkCreateGraphicsPipelines(
            mDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &mSkyPipeline);

        vkDestroyShaderModule(mDevice, vertModule, nullptr);
        vkDestroyShaderModule(mDevice, fragModule, nullptr);

        if (result != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateSkyPipeline: vkCreateGraphicsPipelines failed: {}", static_cast<int>(result));
            return false;
        }

        return true;
    }

    VkSampler VulkanRenderer::AcquireSampler(const SamplerDesc &desc)
    {
        const uint64_t key = (static_cast<uint64_t>(desc.magFilter) << 40) |
                             (static_cast<uint64_t>(desc.minFilter) << 32) |
                             (static_cast<uint64_t>(desc.mipmapMode) << 24) |
                             (static_cast<uint64_t>(desc.addressModeU) << 16) |
                             (static_cast<uint64_t>(desc.addressModeV) << 8) |
                             (desc.useMips ? 1ull : 0ull);

        if (const auto it = mSamplers.find(key); it != mSamplers.end())
            return it->second;

        const VkSamplerCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = desc.magFilter,
            .minFilter = desc.minFilter,
            .mipmapMode = desc.mipmapMode,
            .addressModeU = desc.addressModeU,
            .addressModeV = desc.addressModeV,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .anisotropyEnable = VK_FALSE,
            .compareEnable = VK_FALSE,
            .minLod = 0.0f,
            .maxLod = desc.useMips ? VK_LOD_CLAMP_NONE : 0.0f,
            .borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK };

        VkSampler sampler = VK_NULL_HANDLE;
        if (vkCreateSampler(mDevice, &info, nullptr, &sampler) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateSampler failed");
            return VK_NULL_HANDLE;
        }

        mSamplers.emplace(key, sampler);
        return sampler;
    }

    void VulkanRenderer::DestroySamplers()
    {
        for (const auto &[key, sampler] : mSamplers)
            vkDestroySampler(mDevice, sampler, nullptr);
        mSamplers.clear();
    }

    bool VulkanRenderer::CreateDefaultTextures()
    {
        const std::array<std::byte, 4> white{
            std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 }, std::byte{ 255 } };

        const std::array<std::byte, 4> flatNormal{
            std::byte{ 128 }, std::byte{ 128 }, std::byte{ 255 }, std::byte{ 255 } };

        mDefaultWhiteTexture = CreateTexture(TextureDesc{
            .width = 1, .height = 1, .pixels = white, .srgb = true });
        mDefaultNormalTexture = CreateTexture(TextureDesc{
            .width = 1, .height = 1, .pixels = flatNormal, .srgb = false });

        if (mDefaultWhiteTexture.IsNull() || mDefaultNormalTexture.IsNull())
        {
            MIR_LOG_CRITICAL("CreateDefaultTextures: default texture upload failed");
            return false;
        }

        NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mTextures[mDefaultWhiteTexture.mIndex].mImage), "Default white texture");
        NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(mTextures[mDefaultNormalTexture.mIndex].mImage), "Default normal texture");

        return true;
    }

    bool VulkanRenderer::CreateCameraResources()
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(mPhysicalDevice, &props);
        mCameraAlignedSize = AlignUp(sizeof(CameraData), props.limits.minUniformBufferOffsetAlignment);

        const VkDescriptorSetLayoutBinding binding{
            .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .descriptorCount = 1,
            // Fragment stage added alongside vertex: lit shaders read the
            // light fields per-pixel now (see default.slang).
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT };

        const VkDescriptorSetLayoutCreateInfo setLayoutInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings = &binding };

        if (vkCreateDescriptorSetLayout(mDevice, &setLayoutInfo, nullptr, &mCameraSetLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateCameraResources: vkCreateDescriptorSetLayout failed");
            return false;
        }

        const VkDescriptorPoolSize poolSize{
            .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
            .descriptorCount = kFramesInFlight };

        const VkDescriptorPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = kFramesInFlight,
            .poolSizeCount = 1,
            .pPoolSizes = &poolSize };

        if (vkCreateDescriptorPool(mDevice, &poolInfo, nullptr, &mCameraDescriptorPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateCameraResources: vkCreateDescriptorPool failed");
            return false;
        }

        const VkDeviceSize bufferSize = mCameraAlignedSize * kMaxViewsPerFrame;

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            VmaAllocationInfo allocInfo{};
            if (!CreateBuffer(mAllocator, bufferSize,
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                VMA_ALLOCATION_CREATE_MAPPED_BIT,
                mCameraBuffers[i], mCameraAllocations[i], &allocInfo))
            {
                MIR_LOG_CRITICAL("CreateCameraResources: camera buffer allocation failed for frame {}", i);
                return false;
            }
            mCameraMapped[i] = static_cast<std::byte *>(allocInfo.pMappedData);

            const VkDescriptorSetAllocateInfo setAllocInfo{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = mCameraDescriptorPool,
                .descriptorSetCount = 1,
                .pSetLayouts = &mCameraSetLayout };

            if (vkAllocateDescriptorSets(mDevice, &setAllocInfo, &mCameraDescriptorSets[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("CreateCameraResources: descriptor set allocation failed for frame {}", i);
                return false;
            }

            // range = one view's slice; the dynamic offset walks between them.
            const VkDescriptorBufferInfo bufferInfo{
                .buffer = mCameraBuffers[i],
                .offset = 0,
                .range = sizeof(CameraData) };

            const VkWriteDescriptorSet write{
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = mCameraDescriptorSets[i],
                .dstBinding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                .pBufferInfo = &bufferInfo };

            vkUpdateDescriptorSets(mDevice, 1, &write, 0, nullptr);

            NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mCameraBuffers[i]),
                std::format("Camera UBO {}", i).c_str());
        }

        return true;
    }

    void VulkanRenderer::DestroyCameraResources()
    {
        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (mCameraBuffers[i] != VK_NULL_HANDLE)
            {
                vmaDestroyBuffer(mAllocator, mCameraBuffers[i], mCameraAllocations[i]);
                mCameraBuffers[i] = VK_NULL_HANDLE;
                mCameraAllocations[i] = VK_NULL_HANDLE;
                mCameraMapped[i] = nullptr;
            }
        }

        // Freeing the pool frees its descriptor sets too.
        if (mCameraDescriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(mDevice, mCameraDescriptorPool, nullptr);
            mCameraDescriptorPool = VK_NULL_HANDLE;
        }

        if (mCameraSetLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(mDevice, mCameraSetLayout, nullptr);
            mCameraSetLayout = VK_NULL_HANDLE;
        }
    }

    void VulkanRenderer::WriteCameraData(const glm::mat4 &viewProj, const glm::vec3 &cameraPosWS, uint32_t viewIndex)
    {
        const CameraData data{
            .viewProj = viewProj,
            .lightDirWS = glm::vec4(mLightDirection, 0.0f),
            .lightColor = glm::vec4(mLightColor, 0.0f),
            .ambientColor = glm::vec4(mAmbientColor, 0.0f),
            .invViewProj = glm::inverse(viewProj),
            .cameraPosWS = glm::vec4(cameraPosWS, 1.0f) };
        const VkDeviceSize offset = static_cast<VkDeviceSize>(viewIndex) * mCameraAlignedSize;

        std::byte *const dst = mCameraMapped[mFrameIndex] + offset;
        std::memcpy(dst, &data, sizeof(CameraData));

        // No-op if the memory type is already host-coherent; required
        // otherwise, since nothing else guarantees this write is flushed
        // before the render commands using it are submitted.
        vmaFlushAllocation(mAllocator, mCameraAllocations[mFrameIndex], offset, sizeof(CameraData));
    }

    VkPipeline VulkanRenderer::BuildPipeline(const MaterialDesc &desc)
    {
        VkShaderModule vertModule = CreateShaderModule(mDevice, ShaderPath(desc.shaderName + ".vertexMain.spv"));
        VkShaderModule fragModule = CreateShaderModule(mDevice, ShaderPath(desc.shaderName + ".fragmentMain.spv"));

        if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE)
        {
            if (vertModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice, vertModule, nullptr);
            if (fragModule != VK_NULL_HANDLE)
                vkDestroyShaderModule(mDevice, fragModule, nullptr);
            return VK_NULL_HANDLE;
        }

        const VkPipelineShaderStageCreateInfo stages[]{
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_VERTEX_BIT,
             .module = vertModule,
             .pName = "vertexMain" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
             .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
             .module = fragModule,
             .pName = "fragmentMain" } };

        const VkVertexInputBindingDescription vertexBinding{
            .binding = 0,
            .stride = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };

        const std::array<VkVertexInputAttributeDescription, 6> vertexAttributes{ { { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, pos) },
                                                                                 { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, color) },
                                                                                 { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal) },
                                                                                 { .location = 3, .binding = 0, .format = VK_FORMAT_R32G32_SFLOAT, .offset = offsetof(Vertex, uv) },
                                                                                 { .location = 4, .binding = 0, .format = VK_FORMAT_R32_SFLOAT, .offset = offsetof(Vertex, alpha) },
                                                                                 { .location = 5, .binding = 0, .format = VK_FORMAT_R32G32B32A32_SFLOAT, .offset = offsetof(Vertex, tangent) } } };

        const VkPipelineVertexInputStateCreateInfo vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &vertexBinding,
            // Only the model shader reads uv, alpha and tangent. Declaring
            // them for the others is legal but makes the validation layer
            // complain about attributes no shader consumes.
            .vertexAttributeDescriptionCount =
                desc.shaderName == kModelShaderName ? static_cast<uint32_t>(vertexAttributes.size()) : 3u,
            .pVertexAttributeDescriptions = vertexAttributes.data() };

        const VkPipelineInputAssemblyStateCreateInfo inputAssembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = desc.topology,
            .primitiveRestartEnable = VK_FALSE };

        const VkPipelineViewportStateCreateInfo viewportState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = nullptr,
            .scissorCount = 1,
            .pScissors = nullptr };

        const VkPipelineRasterizationStateCreateInfo rasterizer{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .depthClampEnable = VK_FALSE,
            .rasterizerDiscardEnable = VK_FALSE,
            .polygonMode = desc.polygonMode,
            .cullMode = desc.cullMode,
            .frontFace = desc.frontFace,
            .depthBiasEnable = VK_FALSE,
            .lineWidth = 1.0f };

        const VkPipelineMultisampleStateCreateInfo multisample{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE };

        const VkPipelineColorBlendAttachmentState blendAttachment{
            .blendEnable = desc.alphaMode == MaterialAlphaMode::Blend ? VK_TRUE : VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };

        const VkPipelineDepthStencilStateCreateInfo depthStencil{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = desc.alphaMode == MaterialAlphaMode::Blend ? VK_FALSE : VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_LESS,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE };

        const VkPipelineColorBlendStateCreateInfo colorBlend{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = VK_FALSE,
            .attachmentCount = 1,
            .pAttachments = &blendAttachment };

        const VkDynamicState dynamicStates[]{
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR };

        const VkPipelineDynamicStateCreateInfo dynamicState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamicStates };

        const VkPipelineRenderingCreateInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &mSwapchainFormat,
            .depthAttachmentFormat = kDepthFormat };

        const VkGraphicsPipelineCreateInfo pipelineInfo{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &renderingInfo,
            .stageCount = 2,
            .pStages = stages,
            .pVertexInputState = &vertexInput,
            .pInputAssemblyState = &inputAssembly,
            .pViewportState = &viewportState,
            .pRasterizationState = &rasterizer,
            .pMultisampleState = &multisample,
            .pDepthStencilState = &depthStencil,
            .pColorBlendState = &colorBlend,
            .pDynamicState = &dynamicState,
            .layout = mPipelineLayout,
            .renderPass = VK_NULL_HANDLE,
            .subpass = 0 };

        VkPipeline pipeline = VK_NULL_HANDLE;
        const VkResult result = vkCreateGraphicsPipelines(
            mDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline);

        vkDestroyShaderModule(mDevice, vertModule, nullptr);
        vkDestroyShaderModule(mDevice, fragModule, nullptr);

        if (result != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateGraphicsPipelines failed: {}", static_cast<int>(result));
            return VK_NULL_HANDLE;
        }

        return pipeline;
    }

    MaterialHandle VulkanRenderer::CreateMaterial(const MaterialDesc &desc)
    {
        const VkPipeline pipeline = BuildPipeline(desc);
        if (pipeline == VK_NULL_HANDLE)
            return kNullMaterial;

        const GpuTexture *baseColor = FindTexture(desc.baseColorTexture);
        if (baseColor == nullptr)
            baseColor = FindTexture(mDefaultWhiteTexture);

        const GpuTexture *normal = FindTexture(desc.normalTexture);
        if (normal == nullptr)
            normal = FindTexture(mDefaultNormalTexture);

        if (baseColor == nullptr || normal == nullptr)
        {
            MIR_LOG_CRITICAL("CreateMaterial: default textures unavailable");
            vkDestroyPipeline(mDevice, pipeline, nullptr);
            return kNullMaterial;
        }

        // Shared with every other material that asked for the same state, so
        // this is not destroyed with the material.
        const VkSampler sampler = AcquireSampler(desc.sampler);
        if (sampler == VK_NULL_HANDLE)
        {
            vkDestroyPipeline(mDevice, pipeline, nullptr);
            return kNullMaterial;
        }

        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        const VkDescriptorSetAllocateInfo setAllocInfo{
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = mMaterialDescriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts = &mMaterialSetLayout };

        if (vkAllocateDescriptorSets(mDevice, &setAllocInfo, &descriptorSet) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateMaterial: descriptor set allocation failed");
            vkDestroyPipeline(mDevice, pipeline, nullptr);
            return kNullMaterial;
        }

        const VkDescriptorImageInfo baseColorInfo{
            .sampler = sampler,
            .imageView = baseColor->mView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        const VkDescriptorImageInfo normalInfo{
            .sampler = sampler,
            .imageView = normal->mView,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

        const std::array<VkWriteDescriptorSet, 2> writes{ {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = descriptorSet,
             .dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &baseColorInfo },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
             .dstSet = descriptorSet,
             .dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
             .pImageInfo = &normalInfo } } };

        vkUpdateDescriptorSets(mDevice, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);

        // The model shader reads these out of set 2; see BindModelDescriptors.
        const glm::vec4 parameters{ static_cast<float>(desc.alphaMode), desc.alphaCutoff,
            desc.cullMode == VK_CULL_MODE_NONE ? 1.0f : 0.0f, 0.0f };

        mMaterials.push_back(GpuMaterial{ pipeline, descriptorSet, parameters,
            desc.shaderName == kModelShaderName });

        MIR_LOG_INFO("Material created: {} (index {})", desc.shaderName, mMaterials.size() - 1);
        return MaterialHandle{ static_cast<uint32_t>(mMaterials.size() - 1), 0 };
    }

    void VulkanRenderer::DestroyMaterials()
    {
        for (GpuMaterial &material : mMaterials)
        {
            if (material.mDescriptorSet != VK_NULL_HANDLE)
                vkFreeDescriptorSets(mDevice, mMaterialDescriptorPool, 1, &material.mDescriptorSet);
            if (material.mPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice, material.mPipeline, nullptr);
        }
        mMaterials.clear();
    }

    void VulkanRenderer::ReleaseSceneResources()
    {
        if (mDevice == VK_NULL_HANDLE)
            return;

        // in-flight frames may still use them
        vkDeviceWaitIdle(mDevice);

        const std::size_t meshCount = mMeshes.size();
        DestroyMeshes();
        DestroyModelResources(false);

        // keep the two default textures: CreateMaterial falls back to them
        const std::size_t keptTextures = std::min<std::size_t>(mTextures.size(), 2);
        std::size_t releasedTextures = 0;
        for (std::size_t i = keptTextures; i < mTextures.size(); ++i)
        {
            GpuTexture &texture = mTextures[i];
            if (texture.mView != VK_NULL_HANDLE)
                vkDestroyImageView(mDevice, texture.mView, nullptr);
            if (texture.mImage != VK_NULL_HANDLE)
                vmaDestroyImage(mAllocator, texture.mImage, texture.mAllocation);
            ++releasedTextures;
        }
        mTextures.resize(keptTextures);

        if (mDefaultMaterial.IsNull() || mDefaultMaterial.mIndex >= mMaterials.size())
        {
            const std::size_t materialCount = mMaterials.size();
            DestroyMaterials();
            MIR_LOG_INFO("Released {} meshes, {} materials and {} textures", meshCount, materialCount, releasedTextures);
            return;
        }

        // keep the default material: DrawFrame falls back to it
        const GpuMaterial defaultMaterial = mMaterials[mDefaultMaterial.mIndex];
        std::size_t released = 0;
        for (std::size_t i = 0; i < mMaterials.size(); ++i)
        {
            if (i == mDefaultMaterial.mIndex)
                continue;
            if (mMaterials[i].mDescriptorSet != VK_NULL_HANDLE)
                vkFreeDescriptorSets(mDevice, mMaterialDescriptorPool, 1, &mMaterials[i].mDescriptorSet);
            if (mMaterials[i].mPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice, mMaterials[i].mPipeline, nullptr);
            ++released;
        }

        mMaterials.clear();
        mMaterials.push_back(defaultMaterial);
        mDefaultMaterial = MaterialHandle{ 0, 0 };

        MIR_LOG_INFO("Released {} meshes, {} materials and {} textures", meshCount, released, releasedTextures);
    }

    void VulkanRenderer::ReleaseModelResources(std::span<const MeshHandle> meshes,
        std::span<const MaterialHandle> materials,
        std::span<const TextureHandle> textures)
    {
        if (mDevice == VK_NULL_HANDLE)
            return;

        vkDeviceWaitIdle(mDevice);

        for (const MeshHandle handle : meshes)
        {
            if (handle.IsNull() || handle.mIndex >= mMeshes.size())
                continue;
            GpuMesh &mesh = mMeshes[handle.mIndex];
            if (mesh.mGeneration != handle.mGeneration)
                continue;
            if (mesh.mVertexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mAllocator, mesh.mVertexBuffer, mesh.mVertexAllocation);
            if (mesh.mIndexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mAllocator, mesh.mIndexBuffer, mesh.mIndexAllocation);
            mesh = {};
            // Bumped so a handle to the old contents stops resolving.
            mesh.mGeneration = handle.mGeneration + 1;
        }

        for (const MaterialHandle handle : materials)
        {
            if (handle.IsNull() || handle == mDefaultMaterial || handle.mIndex >= mMaterials.size())
                continue;
            GpuMaterial &material = mMaterials[handle.mIndex];
            // The sampler belongs to the renderer's cache, not to the material.
            if (material.mDescriptorSet != VK_NULL_HANDLE)
                vkFreeDescriptorSets(mDevice, mMaterialDescriptorPool, 1, &material.mDescriptorSet);
            if (material.mPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice, material.mPipeline, nullptr);
            material = {};
        }

        for (const TextureHandle handle : textures)
        {
            // Index 0 and 1 are the defaults every material falls back to.
            if (handle.IsNull() || handle.mIndex < 2 || handle.mIndex >= mTextures.size())
                continue;
            GpuTexture &texture = mTextures[handle.mIndex];
            if (texture.mGeneration != handle.mGeneration)
                continue;
            if (texture.mView != VK_NULL_HANDLE)
                vkDestroyImageView(mDevice, texture.mView, nullptr);
            if (texture.mImage != VK_NULL_HANDLE)
                vmaDestroyImage(mAllocator, texture.mImage, texture.mAllocation);
            texture = {};
            texture.mGeneration = handle.mGeneration + 1;
        }
    }

    const VulkanRenderer::GpuMaterial *VulkanRenderer::FindMaterial(MaterialHandle handle) const
    {
        if (handle.IsNull() || handle.mIndex >= mMaterials.size())
            return nullptr;

        return &mMaterials[handle.mIndex];
    }

    MeshHandle VulkanRenderer::CreateMesh(std::span<const Vertex> vertices,
        std::span<const uint32_t> indices)
    {
        if (vertices.empty() || indices.empty())
        {
            MIR_LOG_ERROR("CreateMesh: vertices and indices must both be non-empty");
            return kNullMesh;
        }

        const VkDeviceSize vertexBytes = vertices.size_bytes();
        const VkDeviceSize indexBytes = indices.size_bytes();

        // temporarily pack multiple data to one buffer for faster creation
        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingInfo{};

        if (!CreateBuffer(mAllocator, vertexBytes + indexBytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            staging, stagingAllocation, &stagingInfo))
        {
            MIR_LOG_CRITICAL("CreateMesh: staging buffer allocation failed");
            return kNullMesh;
        }

        std::byte *const mapped = static_cast<std::byte *>(stagingInfo.pMappedData);
        std::memcpy(mapped, vertices.data(), vertexBytes);
        std::memcpy(mapped + vertexBytes, indices.data(), indexBytes);

        GpuMesh mesh{};
        mesh.mIndexCount = static_cast<uint32_t>(indices.size());
        mesh.mGeneration = 1;

        // No host-access flags on either: that is what puts them in VRAM.
        const bool buffersCreated =
            CreateBuffer(mAllocator, vertexBytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                0, mesh.mVertexBuffer, mesh.mVertexAllocation, nullptr) &&
            CreateBuffer(mAllocator, indexBytes,
                VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                0, mesh.mIndexBuffer, mesh.mIndexAllocation, nullptr);

        if (!buffersCreated)
        {
            MIR_LOG_CRITICAL("CreateMesh: device buffer allocation failed");

            if (mesh.mVertexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mAllocator, mesh.mVertexBuffer, mesh.mVertexAllocation);
            if (mesh.mIndexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mAllocator, mesh.mIndexBuffer, mesh.mIndexAllocation);
            vmaDestroyBuffer(mAllocator, staging, stagingAllocation);
            return kNullMesh;
        }

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        const VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = mGfxQueueFamIdx };

        if (vkCreateCommandPool(mDevice, &poolInfo, nullptr, &uploadPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateMesh: upload command pool creation failed");
            vmaDestroyBuffer(mAllocator, mesh.mVertexBuffer, mesh.mVertexAllocation);
            vmaDestroyBuffer(mAllocator, mesh.mIndexBuffer, mesh.mIndexAllocation);
            vmaDestroyBuffer(mAllocator, staging, stagingAllocation);
            return kNullMesh;
        }

        VkCommandBuffer uploadCmd = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo cmdAllocInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = uploadPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1 };
        vkAllocateCommandBuffers(mDevice, &cmdAllocInfo, &uploadCmd);

        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(uploadCmd, &beginInfo);

        const VkBufferCopy vertexCopy{ .srcOffset = 0, .dstOffset = 0, .size = vertexBytes };
        vkCmdCopyBuffer(uploadCmd, staging, mesh.mVertexBuffer, 1, &vertexCopy);

        // srcOffset picks the second half of the same staging allocation.
        const VkBufferCopy indexCopy{ .srcOffset = vertexBytes, .dstOffset = 0, .size = indexBytes };
        vkCmdCopyBuffer(uploadCmd, staging, mesh.mIndexBuffer, 1, &indexCopy);

        vkEndCommandBuffer(uploadCmd);

        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &uploadCmd };

        vkQueueSubmit(mGfxQueue, 1, &submitInfo, VK_NULL_HANDLE);
        // wait till buffer is created
        vkQueueWaitIdle(mGfxQueue);

        vkDestroyCommandPool(mDevice, uploadPool, nullptr);
        vmaDestroyBuffer(mAllocator, staging, stagingAllocation);

        const uint32_t index = static_cast<uint32_t>(mMeshes.size());
        mMeshes.push_back(mesh);

        NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mesh.mVertexBuffer),
            std::format("Mesh {} vertices", index).c_str());
        NameObject(VK_OBJECT_TYPE_BUFFER, reinterpret_cast<uint64_t>(mesh.mIndexBuffer),
            std::format("Mesh {} indices", index).c_str());

        MIR_LOG_INFO("Mesh {} uploaded: {} vertices, {} indices, {} bytes",
            index, vertices.size(), indices.size(), vertexBytes + indexBytes);

        return MeshHandle{ index, mesh.mGeneration };
    }

    const VulkanRenderer::GpuMesh *VulkanRenderer::FindMesh(MeshHandle handle) const
    {
        if (handle.IsNull() || handle.mIndex >= mMeshes.size())
            return nullptr;

        const GpuMesh &mesh = mMeshes[handle.mIndex];

        if (mesh.mGeneration != handle.mGeneration)
            return nullptr;

        return &mesh;
    }

    void VulkanRenderer::DestroyMeshes()
    {
        for (GpuMesh &mesh : mMeshes)
        {
            if (mesh.mVertexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mAllocator, mesh.mVertexBuffer, mesh.mVertexAllocation);
            if (mesh.mIndexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mAllocator, mesh.mIndexBuffer, mesh.mIndexAllocation);
        }
        mMeshes.clear();
    }

    TextureHandle VulkanRenderer::CreateTexture(const TextureDesc &desc)
    {
        const VkDeviceSize expectedBytes = VkDeviceSize{ desc.width } * desc.height * 4;
        if (desc.width == 0 || desc.height == 0 || desc.pixels.size_bytes() != expectedBytes)
        {
            MIR_LOG_ERROR("CreateTexture: {}x{} expects {} bytes of RGBA8, got {}",
                desc.width, desc.height, expectedBytes, desc.pixels.size_bytes());
            return kNullTexture;
        }

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(mPhysicalDevice, &properties);
        if (desc.width > properties.limits.maxImageDimension2D ||
            desc.height > properties.limits.maxImageDimension2D)
        {
            MIR_LOG_ERROR("CreateTexture: {}x{} is past this device's maxImageDimension2D of {}",
                desc.width, desc.height, properties.limits.maxImageDimension2D);
            return kNullTexture;
        }

        // One staging blob for every level, one copy region each.
        std::vector<std::byte> pixels;
        std::vector<VkBufferImageCopy> regions;
        const uint32_t levelCount = BuildMipChain(desc, pixels, regions);

        VkBuffer staging = VK_NULL_HANDLE;
        VmaAllocation stagingAllocation = VK_NULL_HANDLE;
        VmaAllocationInfo stagingInfo{};

        if (!CreateBuffer(mAllocator, pixels.size(),
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
            VMA_ALLOCATION_CREATE_MAPPED_BIT,
            staging, stagingAllocation, &stagingInfo))
        {
            MIR_LOG_CRITICAL("CreateTexture: staging buffer allocation failed");
            return kNullTexture;
        }

        std::memcpy(stagingInfo.pMappedData, pixels.data(), pixels.size());

        GpuTexture texture{};
        texture.mLevels = levelCount;
        texture.mGeneration = 1;

        const VkFormat format = desc.srgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        const VkImageCreateInfo imageInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent{ desc.width, desc.height, 1 },
            .mipLevels = levelCount,
            .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };

        const VmaAllocationCreateInfo allocInfo{ .usage = VMA_MEMORY_USAGE_AUTO };

        if (vmaCreateImage(mAllocator, &imageInfo, &allocInfo,
            &texture.mImage, &texture.mAllocation, nullptr) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateTexture: vmaCreateImage failed");
            vmaDestroyBuffer(mAllocator, staging, stagingAllocation);
            return kNullTexture;
        }

        VkCommandPool uploadPool = VK_NULL_HANDLE;
        const VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .queueFamilyIndex = mGfxQueueFamIdx };

        if (vkCreateCommandPool(mDevice, &poolInfo, nullptr, &uploadPool) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateTexture: upload command pool creation failed");
            vmaDestroyImage(mAllocator, texture.mImage, texture.mAllocation);
            vmaDestroyBuffer(mAllocator, staging, stagingAllocation);
            return kNullTexture;
        }

        VkCommandBuffer uploadCmd = VK_NULL_HANDLE;
        const VkCommandBufferAllocateInfo cmdAllocInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = uploadPool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1 };
        vkAllocateCommandBuffers(mDevice, &cmdAllocInfo, &uploadCmd);

        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(uploadCmd, &beginInfo);

        ImageBarrier(uploadCmd, texture.mImage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, levelCount);

        vkCmdCopyBufferToImage(uploadCmd, staging, texture.mImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            static_cast<uint32_t>(regions.size()), regions.data());

        ImageBarrier(uploadCmd, texture.mImage, VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, levelCount);

        vkEndCommandBuffer(uploadCmd);

        const VkSubmitInfo submitInfo{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1,
            .pCommandBuffers = &uploadCmd };

        vkQueueSubmit(mGfxQueue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(mGfxQueue);

        vkDestroyCommandPool(mDevice, uploadPool, nullptr);
        vmaDestroyBuffer(mAllocator, staging, stagingAllocation);

        const VkImageViewCreateInfo viewInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = texture.mImage,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = format,
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = levelCount,
                .baseArrayLayer = 0,
                .layerCount = 1 } };

        if (vkCreateImageView(mDevice, &viewInfo, nullptr, &texture.mView) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("CreateTexture: vkCreateImageView failed");
            vmaDestroyImage(mAllocator, texture.mImage, texture.mAllocation);
            return kNullTexture;
        }

        const uint32_t index = static_cast<uint32_t>(mTextures.size());
        mTextures.push_back(texture);

        NameObject(VK_OBJECT_TYPE_IMAGE, reinterpret_cast<uint64_t>(texture.mImage),
            std::format("Texture {}", index).c_str());

        MIR_LOG_INFO("Texture {} uploaded: {}x{}, {} level(s), {} bytes",
            index, desc.width, desc.height, levelCount, pixels.size());

        return TextureHandle{ index, texture.mGeneration };
    }

    const VulkanRenderer::GpuTexture *VulkanRenderer::FindTexture(TextureHandle handle) const
    {
        if (handle.IsNull() || handle.mIndex >= mTextures.size())
            return nullptr;

        const GpuTexture &texture = mTextures[handle.mIndex];

        if (texture.mGeneration != handle.mGeneration)
            return nullptr;

        return &texture;
    }

    void VulkanRenderer::DestroyTextures()
    {
        for (GpuTexture &texture : mTextures)
        {
            if (texture.mView != VK_NULL_HANDLE)
                vkDestroyImageView(mDevice, texture.mView, nullptr);
            if (texture.mImage != VK_NULL_HANDLE)
                vmaDestroyImage(mAllocator, texture.mImage, texture.mAllocation);
        }
        mTextures.clear();
        mDefaultWhiteTexture = kNullTexture;
        mDefaultNormalTexture = kNullTexture;
    }

    void VulkanRenderer::NameObject(VkObjectType type, uint64_t handle, const char *name)
    {
        // only when debugging
        if (!mValidationEnabled || handle == 0)
            return;

        const VkDebugUtilsObjectNameInfoEXT info{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType = type,
            .objectHandle = handle,
            .pObjectName = name };

        vkSetDebugUtilsObjectNameEXT(mDevice, &info);
    }

    bool VulkanRenderer::CreateFrameResources()
    {
        const VkCommandPoolCreateInfo poolInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = 0,
            .queueFamilyIndex = mGfxQueueFamIdx };

        const VkSemaphoreCreateInfo semInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

        for (uint32_t i = 0; i < kFramesInFlight; ++i)
        {
            if (vkCreateCommandPool(mDevice, &poolInfo, nullptr, &mCmdPools[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vkCreateCommandPool failed for frame {}", i);
                return false;
            }

            const VkCommandBufferAllocateInfo allocInfo{
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = mCmdPools[i],
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1 };

            if (vkAllocateCommandBuffers(mDevice, &allocInfo, &mCmdBuffers[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("vkAllocateCommandBuffers failed for frame {}", i);
                return false;
            }

            if (vkCreateSemaphore(mDevice, &semInfo, nullptr, &mImageAcquired[i]) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("image-acquired semaphore failed for frame {}", i);
                return false;
            }
        }

        // Paired with mNextSignalValue = kFramesInFlight + 1
        const VkSemaphoreTypeCreateInfo typeInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = kFramesInFlight };

        const VkSemaphoreCreateInfo timelineInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &typeInfo };

        if (vkCreateSemaphore(mDevice, &timelineInfo, nullptr, &mTimeline) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("timeline semaphore creation failed");
            return false;
        }

        return CreateRenderCompleteSemaphores();
    }

    bool VulkanRenderer::CreateRenderCompleteSemaphores()
    {
        const VkSemaphoreCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };

        mRenderComplete.resize(mSwapchainImages.size(), VK_NULL_HANDLE);

        for (VkSemaphore &sem : mRenderComplete)
        {
            if (vkCreateSemaphore(mDevice, &info, nullptr, &sem) != VK_SUCCESS)
            {
                MIR_LOG_CRITICAL("render-complete semaphore creation failed");
                return false;
            }
        }
        return true;
    }

    void VulkanRenderer::DestroyRenderCompleteSemaphores()
    {
        for (VkSemaphore sem : mRenderComplete)
        {
            if (sem != VK_NULL_HANDLE)
                vkDestroySemaphore(mDevice, sem, nullptr);
        }
        mRenderComplete.clear();
    }
    void VulkanRenderer::RecordView(VkCommandBuffer cmd, const RenderView &view, uint32_t viewIndex)
    {
        const VkRect2D requestedRect = (view.rect.extent.width > 0 && view.rect.extent.height > 0)
            ? view.rect
            : VkRect2D{ .offset{ 0, 0 }, .extent = mSwapchainExtent };
        VkRect2D rect = requestedRect;
        rect.offset.x = std::clamp(rect.offset.x, 0, static_cast<int32_t>(mSwapchainExtent.width));
        rect.offset.y = std::clamp(rect.offset.y, 0, static_cast<int32_t>(mSwapchainExtent.height));
        rect.extent.width = std::min(rect.extent.width,
            mSwapchainExtent.width - static_cast<uint32_t>(rect.offset.x));
        rect.extent.height = std::min(rect.extent.height,
            mSwapchainExtent.height - static_cast<uint32_t>(rect.offset.y));

        const VkViewport viewport{
            .x = static_cast<float>(rect.offset.x),
            .y = static_cast<float>(rect.offset.y),
            .width = static_cast<float>(std::max(rect.extent.width, 1u)),
            .height = static_cast<float>(std::max(rect.extent.height, 1u)),
            .minDepth = 0.0f,
            .maxDepth = 1.0f };
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &rect);

        WriteCameraData(view.viewProj, view.cameraPos, viewIndex);
        const uint32_t cameraOffset = static_cast<uint32_t>(viewIndex * mCameraAlignedSize);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout,
            1, 1, &mCameraDescriptorSets[mFrameIndex], 1, &cameraOffset);

        VkPipeline boundPipeline = VK_NULL_HANDLE;
        VkDescriptorSet boundDescriptorSet = VK_NULL_HANDLE;

        // no mesh/material batching yet, just draw per item
        for (const DrawItem &item : view.items)
        {
            const GpuMesh *const mesh = FindMesh(item.mesh);
            if (mesh == nullptr)
                continue;

            const GpuMaterial *material = FindMaterial(item.material);
            // A slot emptied by ReleaseModelResources is still in range, so
            // check the pipeline rather than the handle alone.
            if (material == nullptr || material->mPipeline == VK_NULL_HANDLE)
                material = FindMaterial(mDefaultMaterial);
            if (material == nullptr || material->mPipeline == VK_NULL_HANDLE)
                continue;

            if (material->mModelShader && !BindModelDescriptors(cmd, *material, item))
                continue;

            if (material->mPipeline != boundPipeline)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, material->mPipeline);
                boundPipeline = material->mPipeline;
            }

            if (material->mDescriptorSet != boundDescriptorSet)
            {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mPipelineLayout,
                    0, 1, &material->mDescriptorSet, 0, nullptr);
                boundDescriptorSet = material->mDescriptorSet;
            }

            const VkDeviceSize vertexOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->mVertexBuffer, &vertexOffset);

            vkCmdBindIndexBuffer(cmd, mesh->mIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

            const PushData pushData{
                .model = item.model,
                .normalCol0 = glm::vec4(item.normalMatrix[0], 0.0f),
                .normalCol1 = glm::vec4(item.normalMatrix[1], 0.0f),
                .normalCol2 = glm::vec4(item.normalMatrix[2], 0.0f),
                .tint = item.tint };

            vkCmdPushConstants(cmd, mPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                0, sizeof(PushData), &pushData);

            vkCmdDrawIndexed(cmd, mesh->mIndexCount, 1, 0, 0, 0);
        }

        // Sky last: depth test (LEQUAL, no write) means the fragment shader
        // only runs where nothing above wrote depth, i.e. still at the far
        // clear value - a fullscreen ray-marched shader paying its cost only
        // on actual background pixels, with early-z doing the rejection.
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mSkyPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, mSkyPipelineLayout,
            0, 1, &mCameraDescriptorSets[mFrameIndex], 1, &cameraOffset);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }

    void VulkanRenderer::RecordCommands(VkCommandBuffer cmd, uint32_t imageIndex, std::span<const RenderView> views)
    {
        // get image ready
        ImageBarrier(cmd, mSwapchainImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        ImageBarrier(cmd, mDepthImages[mFrameIndex], VK_IMAGE_ASPECT_DEPTH_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

        const VkRenderingAttachmentInfo colorAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = mSwapchainViews[imageIndex],
            .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .clearValue{ .color{ { mClearColor.r, mClearColor.g, mClearColor.b, mClearColor.a } } } };

        const VkRenderingAttachmentInfo depthAttachment{
            .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
            .imageView = mDepthViews[mFrameIndex],
            .imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .clearValue{ .depthStencil{ .depth = 1.0f } } };

        const VkRenderingInfo renderingInfo{
            .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
            // The stored extent, never the window: re-deriving it invites drift.
            .renderArea{ .offset{ 0, 0 }, .extent = mSwapchainExtent },
            .layerCount = 1,
            .colorAttachmentCount = 1,
            .pColorAttachments = &colorAttachment,
            .pDepthAttachment = &depthAttachment };

        vkCmdBeginRendering(cmd, &renderingInfo);

        if (mValidationEnabled)
        {
            const VkDebugUtilsLabelEXT label{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                .pLabelName = "Triangle Pass",
                .color{ 1.0f, 0.0f, 1.0f, 1.0f } };
            vkCmdBeginDebugUtilsLabelEXT(cmd, &label);
        }

        if (views.size() > kMaxViewsPerFrame)
        {
            MIR_LOG_ERROR("RecordCommands: {} views this frame, only the first {} (kMaxViewsPerFrame) are drawn",
                views.size(), kMaxViewsPerFrame);
        }

        const size_t viewCount = std::min<size_t>(views.size(), kMaxViewsPerFrame);
        for (size_t i = 0; i < viewCount; ++i)
            RecordView(cmd, views[i], static_cast<uint32_t>(i));

        if (mImguiDrawData != nullptr)
            ImGui_ImplVulkan_RenderDrawData(mImguiDrawData, cmd);

        if (mValidationEnabled)
            vkCmdEndDebugUtilsLabelEXT(cmd);

        vkCmdEndRendering(cmd);

        const bool captured = RecordCapture(cmd, imageIndex);

        // give it back
        ImageBarrier(cmd, mSwapchainImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
            captured ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            captured ? VK_PIPELINE_STAGE_2_TRANSFER_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            captured ? VK_ACCESS_2_TRANSFER_READ_BIT : VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            VK_PIPELINE_STAGE_2_NONE, 0);
    }

    void VulkanRenderer::DrawFrame(std::span<const RenderView> views)
    {
        if (mWindow->Width() == 0 || mWindow->Height() == 0)
            return;

        // check swapchain creation
        if (mNeedRecreate || mWindow->Height() != mSwapchainExtent.height || mWindow->Width() != mSwapchainExtent.width)
        {
            if (!RecreateSwapchain())
                return;
        }

        const uint64_t signalValue = mNextSignalValue++;
        const uint64_t waitValue = signalValue - kFramesInFlight;

        const VkSemaphoreWaitInfo waitInfo{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
            .semaphoreCount = 1,
            .pSemaphores = &mTimeline,
            .pValues = &waitValue };

        // wait for next image to comeback from framesin flight
        vkWaitSemaphores(mDevice, &waitInfo, UINT64_MAX);
        ResetModelFrame(mFrameIndex);

        // Whole-pool reset, not per-buffer.
        vkResetCommandPool(mDevice, mCmdPools[mFrameIndex], 0);

        uint32_t imageIndex = 0;
        const VkResult acquireResult = vkAcquireNextImageKHR(
            mDevice, mSwapchain, UINT64_MAX,
            mImageAcquired[mFrameIndex], VK_NULL_HANDLE, &imageIndex);

        // The semaphore was not signalled, so this frame cannot proceed.
        if (acquireResult == VK_ERROR_OUT_OF_DATE_KHR)
        {
            // Roll the signal value back: nothing will signal it.
            mNeedRecreate = true;
            --mNextSignalValue;
            return;
        }

        // Signalled and usable. Finish the frame, recreate next one.
        if (acquireResult == VK_SUBOPTIMAL_KHR)
        {
            mNeedRecreate = true;
        }
        else if (acquireResult != VK_SUCCESS)
        {
            MIR_LOG_ERROR("vkAcquireNextImageKHR failed: {}", static_cast<int>(acquireResult));
            --mNextSignalValue;
            return;
        }

        VkCommandBuffer cmd = mCmdBuffers[mFrameIndex];

        const VkCommandBufferBeginInfo beginInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };

        vkBeginCommandBuffer(cmd, &beginInfo);
        RecordCommands(cmd, imageIndex, views);
        vkEndCommandBuffer(cmd);

        const VkSemaphoreSubmitInfo waitSem{
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
            .semaphore = mImageAcquired[mFrameIndex],
            .stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT };

        const VkSemaphoreSubmitInfo signalSems[]{
            { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
             .semaphore = mRenderComplete[imageIndex],
             .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT },
            { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
             .semaphore = mTimeline,
             .value = signalValue,
             .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT } };

        const VkCommandBufferSubmitInfo cmdInfo{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
            .commandBuffer = cmd };

        const VkSubmitInfo2 submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
            .waitSemaphoreInfoCount = 1,
            .pWaitSemaphoreInfos = &waitSem,
            .commandBufferInfoCount = 1,
            .pCommandBufferInfos = &cmdInfo,
            .signalSemaphoreInfoCount = 2,
            .pSignalSemaphoreInfos = signalSems };

        if (vkQueueSubmit2(mGfxQueue, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS)
        {
            MIR_LOG_ERROR("vkQueueSubmit2 failed");
            return;
        }
        if (mCaptureBuffer != VK_NULL_HANDLE) FinishCapture();
        const VkPresentInfoKHR presentInfo{
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &mRenderComplete[imageIndex],
            .swapchainCount = 1,
            .pSwapchains = &mSwapchain,
            .pImageIndices = &imageIndex };

        const VkResult presentResult = vkQueuePresentKHR(mGfxQueue, &presentInfo);
        if (presentResult == VK_ERROR_OUT_OF_DATE_KHR || presentResult == VK_SUBOPTIMAL_KHR)
            mNeedRecreate = true;
        else if (presentResult != VK_SUCCESS)
            MIR_LOG_ERROR("vkQueuePresentKHR failed: {}", static_cast<int>(presentResult));

        mFrameIndex = (mFrameIndex + 1) % kFramesInFlight;
    }
}
