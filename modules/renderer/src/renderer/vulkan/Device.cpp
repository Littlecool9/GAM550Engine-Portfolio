/**
 * @file Device.cpp
 * @author Sumin Park
 * @brief Vulkan instance, debug messenger, surface, physical and logical device, allocator.
 *
 * @copyright Copyright (c) 2026 DigiPen (USA) Corporation
 *
 */
#include "Device.h"
#include "VulkanSurface.h"

#include <renderer/VulkanRenderer.h>

#include <core/log/Log.h>
#include <core/log/Assert.h>

#include <vk_mem_alloc.h>

#include <cstring>
#include <vector>

namespace mir::vk
{
    namespace
    {
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
                features11.shaderDrawParameters == VK_TRUE &&
                features.features.imageCubeArray == VK_TRUE;
        }

        bool HasRayTracingSupport(VkPhysicalDevice device,
            VkPhysicalDeviceRayTracingPipelinePropertiesKHR &outProps)
        {
            if (!HasDeviceExtension(device, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) ||
                !HasDeviceExtension(device, VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME) ||
                !HasDeviceExtension(device, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME))
                return false;

            VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR };

            VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
                .pNext = &rtPipelineFeatures };

            VkPhysicalDeviceFeatures2 features{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
                .pNext = &accelFeatures };

            vkGetPhysicalDeviceFeatures2(device, &features);

            if (accelFeatures.accelerationStructure != VK_TRUE ||
                rtPipelineFeatures.rayTracingPipeline != VK_TRUE)
                return false;

            outProps = VkPhysicalDeviceRayTracingPipelinePropertiesKHR{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_PROPERTIES_KHR };

            VkPhysicalDeviceProperties2 props{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = &outProps };

            vkGetPhysicalDeviceProperties2(device, &props);
            return true;
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
    }

    bool Device::Initialize(const RendererDesc &desc)
    {
        if (volkInitialize() != VK_SUCCESS)
        {
            MIR_LOG_ERROR("volk initialized failed");
            return false;
        }

        if (!CreateInstance(desc))
            return false;

        volkLoadInstance(mInstance);

        if (mValidationEnabled && !CreateDebugMessenger())
            return false;

        if (!CreateSurface(desc))
            return false;

        if (!FindPhysicalDevice())
            return false;

        if (!CreateLogicalDevice())
            return false;

        if (!CreateAllocator())
            return false;

        NameObject(VK_OBJECT_TYPE_DEVICE, reinterpret_cast<uint64_t>(mDevice), "MjolnirEngine device");
        NameObject(VK_OBJECT_TYPE_QUEUE, reinterpret_cast<uint64_t>(mQueue), "Graphics+present queue");

        return true;
    }

    void Device::Shutdown()
    {
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
            vkDestroyDebugUtilsMessengerEXT(mInstance, mDebugMessenger, nullptr);
            mDebugMessenger = VK_NULL_HANDLE;
        }

        if (mSurface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(mInstance, mSurface, nullptr);
            mSurface = VK_NULL_HANDLE;
        }

        if (mInstance != VK_NULL_HANDLE)
        {
            vkDestroyInstance(mInstance, nullptr);
            mInstance = VK_NULL_HANDLE;
        }

        volkFinalize();
    }

    bool Device::CreateInstance(const RendererDesc &desc)
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

        const WindowBackend backend = desc.window->NativeWindow().backend;

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
            .apiVersion = kApiVersion };

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

        if (vkCreateInstance(&createInfo, nullptr, &mInstance) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateInstance failed");
            return false;
        }

        return true;
    }
    bool Device::CreateDebugMessenger()
    {
        const VkDebugUtilsMessengerCreateInfoEXT info = MakeMessengerCreateInfo();

        if (vkCreateDebugUtilsMessengerEXT(mInstance, &info, nullptr, &mDebugMessenger) != VK_SUCCESS)
        {
            MIR_LOG_ERROR("vkCreateDebugUtilsMessengerEXT failed");
            return false;
        }
        return true;
    }
    bool Device::CreateSurface(const RendererDesc &desc)
    {
        mSurface = vk::CreateSurface(mInstance, desc.window->NativeWindow());

        if (mSurface == VK_NULL_HANDLE)
        {
            MIR_LOG_CRITICAL("Vulkan surface creation failed");
            return false;
        }
        return true;
    }

    bool Device::FindPhysicalDevice()
    {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(mInstance, &count, nullptr);
        if (count == 0)
        {
            MIR_LOG_CRITICAL("No Vulkan-capable GPU found");
            return false;
        }

        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(mInstance, &count, devices.data());

        uint32_t bestScore = 0;

        for (VkPhysicalDevice device : devices)
        {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(device, &props);

            // Every rejection says which device and why
            if (props.apiVersion < kApiVersion)
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
                mPhysical = device;
                mQueueFamily = family;
            }
        }

        if (mPhysical == VK_NULL_HANDLE)
        {
            MIR_LOG_CRITICAL("No suitable GPU among {} candidate(s)", count);
            return false;
        }

        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(mPhysical, &props);
        MIR_LOG_INFO("GPU: {} | {} | Vulkan {}.{}.{} | graphics+present family {}",
            props.deviceName,
            props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete" : "integrated",
            VK_API_VERSION_MAJOR(props.apiVersion),
            VK_API_VERSION_MINOR(props.apiVersion),
            VK_API_VERSION_PATCH(props.apiVersion),
            mQueueFamily);

        mRayTracingSupported = HasRayTracingSupport(mPhysical, mRtProperties);
        if (mRayTracingSupported)
            MIR_LOG_INFO("Ray tracing: {} supports VK_KHR_acceleration_structure + VK_KHR_ray_tracing_pipeline", props.deviceName);
        else
            MIR_LOG_WARN("Ray tracing unavailable on {}: missing the acceleration-structure/ray-tracing-pipeline "
                "extensions or feature bits - raster only", props.deviceName);

        return true;
    }

    bool Device::CreateLogicalDevice()
    {
        // required even for single queue, needed in infostruct
        const float queuePriority = 1.0f;

        const VkDeviceQueueCreateInfo queueInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = mQueueFamily,
            .queueCount = 1,
            .pQueuePriorities = &queuePriority };

        // list of extensions at 1.4
        // dynamic rendering, sync resources are core, not extensions
        // .pNext to link multiple extensions together
        std::vector<const char *> deviceExtensions{ VK_KHR_SWAPCHAIN_EXTENSION_NAME };
        if (mRayTracingSupported)
        {
            deviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_RAY_TRACING_PIPELINE_EXTENSION_NAME);
        }

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

        VkPhysicalDeviceRayTracingPipelineFeaturesKHR rtPipelineFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_TRACING_PIPELINE_FEATURES_KHR,
            .pNext = &features11,
            .rayTracingPipeline = VK_TRUE };

        VkPhysicalDeviceAccelerationStructureFeaturesKHR accelFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR,
            .pNext = &rtPipelineFeatures,
            .accelerationStructure = VK_TRUE };

        VkPhysicalDeviceFeatures2 features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
            .pNext = mRayTracingSupported ? static_cast<void *>(&accelFeatures) : static_cast<void *>(&features11),
            .features{ .imageCubeArray = VK_TRUE } };

        const VkDeviceCreateInfo deviceInfo{
            .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext = &features,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueInfo,
            .enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size()),
            .ppEnabledExtensionNames = deviceExtensions.data(),
            .pEnabledFeatures = nullptr // must be nullptr, everything goes through .pNext
        };

        if (vkCreateDevice(mPhysical, &deviceInfo, nullptr, &mDevice) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vkCreateDevice Failed");
            return false;
        }

        volkLoadDevice(mDevice);
        vkGetDeviceQueue(mDevice, mQueueFamily, 0, &mQueue);

        return true;
    }
    bool Device::CreateAllocator()
    {
        const VmaVulkanFunctions functions{
            .vkGetInstanceProcAddr = vkGetInstanceProcAddr,
            .vkGetDeviceProcAddr = vkGetDeviceProcAddr };

        const VmaAllocatorCreateInfo allocatorInfo{
            .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
            .physicalDevice = mPhysical,
            .device = mDevice,
            .pVulkanFunctions = &functions,
            .instance = mInstance,
            .vulkanApiVersion = kApiVersion };

        if (vmaCreateAllocator(&allocatorInfo, &mAllocator) != VK_SUCCESS)
        {
            MIR_LOG_CRITICAL("vmaCreateAllocator failed");
            return false;
        }
        return true;
    }

    void Device::NameObject(VkObjectType type, uint64_t handle, const char *name) const
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
}
