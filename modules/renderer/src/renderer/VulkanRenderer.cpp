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

#include <vk_mem_alloc.h>

#include <imgui.h>
#include <imgui_impl_vulkan.h>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <vector>
#include <cstring>
#include <algorithm>
#include <fstream>
#include <optional>
#include <array>
#include <chrono>
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
            glm::mat4 transform;  // 64
            glm::vec4 normalCol0; // 16
            glm::vec4 normalCol1; // 16
            glm::vec4 normalCol2; // 16
            glm::vec4 tint;       // 16
        };
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
            VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
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
                    .levelCount = 1,
                    .layerCount = 1 } };

            const VkDependencyInfo dep{
                .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
                .imageMemoryBarrierCount = 1,
                .pImageMemoryBarriers = &barrier };

            vkCmdPipelineBarrier2(cmd, &dep);
        }
        std::optional<std::vector<uint32_t>> ReadSpirv(const std::filesystem::path &path)
        {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file)
            {
                MIR_LOG_CRITICAL("Cannot open SPIR-V: {}", path.string());
                return std::nullopt;
            }

            const std::streamsize byteSize = file.tellg();
            if (byteSize <= 0 || byteSize % 4 != 0)
            {
                MIR_LOG_CRITICAL("Bad SPIR-V size ({} bytes): {}", byteSize, path.string());
                return std::nullopt;
            }

            std::vector<uint32_t> words(static_cast<size_t>(byteSize) / 4);
            file.seekg(0);
            file.read(reinterpret_cast<char *>(words.data()), byteSize);

            if (!file)
            {
                MIR_LOG_CRITICAL("Short read on SPIR-V: {}", path.string());
                return std::nullopt;
            }
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

        if (!CreatePipelineLayout())
            return false;

        mDefaultMaterial = CreateMaterial(MaterialDesc{});
        if (mDefaultMaterial.IsNull())
            return false;

        NameObject(VK_OBJECT_TYPE_DEVICE, reinterpret_cast<uint64_t>(mDevice), "MjolnirEngine device");
        NameObject(VK_OBJECT_TYPE_QUEUE, reinterpret_cast<uint64_t>(mGfxQueue), "Graphics+present queue");
        NameObject(VK_OBJECT_TYPE_SWAPCHAIN_KHR, reinterpret_cast<uint64_t>(mSwapchain), "Swapchain");
        NameObject(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(mMaterials[mDefaultMaterial.mIndex].mPipeline), "Default material pipeline");

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
            DestroyMaterials();

            if (mPipelineLayout != VK_NULL_HANDLE)
            {
                vkDestroyPipelineLayout(mDevice, mPipelineLayout, nullptr);
                mPipelineLayout = VK_NULL_HANDLE;
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
                MIR_LOG_INFO("Rejected {}: Vulkan {}.{}, need 1.3", props.deviceName,
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

        // list of extensions at 1.3
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
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
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
        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset = 0,
            .size = sizeof(PushData) };

        const VkPipelineLayoutCreateInfo layoutInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 0,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &pushConstantRange };

        if (vkCreatePipelineLayout(mDevice, &layoutInfo, nullptr, &mPipelineLayout) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreatePipelineLayout failed");
            return false;
        }

        return true;
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

        const std::array<VkVertexInputAttributeDescription, 3> vertexAttributes{ { { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, pos) },
                                                                                 { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, color) },
                                                                                 { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(Vertex, normal) } } };

        const VkPipelineVertexInputStateCreateInfo vertexInput{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &vertexBinding,
            .vertexAttributeDescriptionCount = static_cast<uint32_t>(vertexAttributes.size()),
            .pVertexAttributeDescriptions = vertexAttributes.data() };

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
            .polygonMode = desc.polygonMode,
            .cullMode = desc.cullMode,
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
            .depthWriteEnable = VK_TRUE,
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

        mMaterials.push_back(GpuMaterial{ pipeline });

        MIR_LOG_INFO("Material created: {} (index {})", desc.shaderName, mMaterials.size() - 1);
        return MaterialHandle{ static_cast<uint32_t>(mMaterials.size() - 1), 0 };
    }

    void VulkanRenderer::DestroyMaterials()
    {
        for (GpuMaterial &material : mMaterials)
        {
            if (material.mPipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(mDevice, material.mPipeline, nullptr);
        }
        mMaterials.clear();
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

        // MAPPED_BIT means pMappedData is already valid - no vmaMapMemory or
        // matching unmap. SEQUENTIAL_WRITE says these writes are forward-only,
        // which lets VMA pick write-combined memory; never read this pointer.
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
            // The && above short-circuits, so one of the two may be null here.
            // Both destroys are guarded, so this handles either case.
            if (mesh.mVertexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mAllocator, mesh.mVertexBuffer, mesh.mVertexAllocation);
            if (mesh.mIndexBuffer != VK_NULL_HANDLE)
                vmaDestroyBuffer(mAllocator, mesh.mIndexBuffer, mesh.mIndexAllocation);
            vmaDestroyBuffer(mAllocator, staging, stagingAllocation);
            return kNullMesh;
        }

        // Throwaway pool, not a frame pool: the frame loop resets those, and
        // this may run before the loop has started.
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
    void VulkanRenderer::RecordView(VkCommandBuffer cmd, const RenderView &view)
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

        VkPipeline boundPipeline = VK_NULL_HANDLE;

        // no mesh/material batching yet, just draw per item
        for (const DrawItem &item : view.items)
        {
            const GpuMesh *const mesh = FindMesh(item.mesh);
            if (mesh == nullptr)
                continue;

            const GpuMaterial *material = FindMaterial(item.material);
            if (material == nullptr)
                material = FindMaterial(mDefaultMaterial);
            if (material == nullptr)
                continue;

            if (material->mPipeline != boundPipeline)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, material->mPipeline);
                boundPipeline = material->mPipeline;
            }

            const VkDeviceSize vertexOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &mesh->mVertexBuffer, &vertexOffset);

            vkCmdBindIndexBuffer(cmd, mesh->mIndexBuffer, 0, VK_INDEX_TYPE_UINT32);

            const PushData pushData{
                .transform = item.model,
                .normalCol0 = glm::vec4(item.normalMatrix[0], 0.0f),
                .normalCol1 = glm::vec4(item.normalMatrix[1], 0.0f),
                .normalCol2 = glm::vec4(item.normalMatrix[2], 0.0f),
                .tint = item.tint };

            vkCmdPushConstants(cmd, mPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT,
                0, sizeof(PushData), &pushData);

            vkCmdDrawIndexed(cmd, mesh->mIndexCount, 1, 0, 0, 0);
        }
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
        -
            for (const RenderView &view : views)
                RecordView(cmd, view);

        if (mImguiDrawData != nullptr)
            ImGui_ImplVulkan_RenderDrawData(mImguiDrawData, cmd);

        if (mValidationEnabled)
            vkCmdEndDebugUtilsLabelEXT(cmd);

        vkCmdEndRendering(cmd);

        // give it back
        ImageBarrier(cmd, mSwapchainImages[imageIndex], VK_IMAGE_ASPECT_COLOR_BIT,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
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
