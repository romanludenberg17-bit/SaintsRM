#define VK_USE_PLATFORM_ANDROID_KHR
#include <jni.h>
#include <vulkan/vulkan.h>

#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool has_extension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
    for (const auto& extension : extensions) {
        if (std::strcmp(extension.extensionName, name) == 0) return true;
    }
    return false;
}

void append_instance_extensions(std::ostringstream& out) {
    uint32_t count = 0;
    VkResult result = vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr);
    out << "Vulkan instance extensions (" << count << "):\n";
    if (result != VK_SUCCESS) {
        out << "  <enumeration failed: " << result << ">\n";
        return;
    }

    std::vector<VkExtensionProperties> extensions(count);
    result = vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data());
    if (result != VK_SUCCESS) {
        out << "  <enumeration failed: " << result << ">\n";
        return;
    }

    for (const auto& extension : extensions) {
        out << "  " << extension.extensionName << " (" << extension.specVersion << ")\n";
    }
}

void append_device_report(std::ostringstream& out, VkPhysicalDevice device) {
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceFeatures legacy_features{};
    vkGetPhysicalDeviceProperties(device, &properties);
    vkGetPhysicalDeviceFeatures(device, &legacy_features);

    out << "GPU deviceName: " << properties.deviceName << "\n";
    out << "GPU vendorID: 0x" << std::hex << properties.vendorID << std::dec << "\n";
    out << "GPU deviceID: 0x" << std::hex << properties.deviceID << std::dec << "\n";
    out << "Vulkan API version: "
        << VK_VERSION_MAJOR(properties.apiVersion) << "."
        << VK_VERSION_MINOR(properties.apiVersion) << "."
        << VK_VERSION_PATCH(properties.apiVersion) << "\n";
    out << "Vulkan driverVersion: 0x" << std::hex << properties.driverVersion << std::dec << "\n";
    out << "features.geometryShader: " << (legacy_features.geometryShader ? "true" : "false") << "\n";
    out << "features.samplerAnisotropy: " << (legacy_features.samplerAnisotropy ? "true" : "false") << "\n";

    uint32_t extension_count = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(device, nullptr, &extension_count, nullptr);
    std::vector<VkExtensionProperties> extensions;
    if (result == VK_SUCCESS) {
        extensions.resize(extension_count);
        result = vkEnumerateDeviceExtensionProperties(
            device, nullptr, &extension_count, extensions.data());
    }

    out << "Vulkan device extensions (" << extension_count << "):\n";
    if (result == VK_SUCCESS) {
        for (const auto& extension : extensions) {
            out << "  " << extension.extensionName << " (" << extension.specVersion << ")\n";
        }
    } else {
        out << "  <enumeration failed: " << result << ">\n";
    }

    const uint32_t api_version = properties.apiVersion;
    const bool has_vulkan11 =
        VK_VERSION_MAJOR(api_version) > 1 ||
        (VK_VERSION_MAJOR(api_version) == 1 && VK_VERSION_MINOR(api_version) >= 1);
    const bool has_vulkan12 =
        VK_VERSION_MAJOR(api_version) > 1 ||
        (VK_VERSION_MAJOR(api_version) == 1 && VK_VERSION_MINOR(api_version) >= 2);
    const bool has_interlock =
        has_extension(extensions, VK_EXT_FRAGMENT_SHADER_INTERLOCK_EXTENSION_NAME);
    const bool has_float_controls =
        has_vulkan12 || has_extension(extensions, VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME);
    const bool has_properties2_extension =
        has_extension(extensions, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
    const bool can_query_features2 = has_vulkan11 || has_properties2_extension;

    out << "capabilityQuery.features2: "
        << (can_query_features2 ? "available" : "unavailable") << "\n";
    out << "capabilityQuery.fragmentShaderInterlockExtension: "
        << (has_interlock ? "advertised" : "not advertised") << "\n";
    out << "capabilityQuery.shaderFloatControls: "
        << (has_float_controls ? "advertised/core" : "not advertised") << "\n";

    VkPhysicalDeviceVulkan12Features vulkan12{};
    vulkan12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;

    VkPhysicalDeviceFragmentShaderInterlockFeaturesEXT interlock{};
    interlock.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_INTERLOCK_FEATURES_EXT;

    if (can_query_features2) {
        VkPhysicalDeviceFeatures2 features2{};
        features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

        VkBaseOutStructure* tail = reinterpret_cast<VkBaseOutStructure*>(&features2);
        if (has_vulkan12) {
            tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&vulkan12);
            tail = reinterpret_cast<VkBaseOutStructure*>(&vulkan12);
        }
        if (has_interlock) {
            tail->pNext = reinterpret_cast<VkBaseOutStructure*>(&interlock);
        }

        vkGetPhysicalDeviceFeatures2(device, &features2);

        if (has_vulkan12) {
            out << "features.descriptorIndexing: "
                << (vulkan12.descriptorIndexing ? "true" : "false") << "\n";
            out << "features.shaderSampledImageArrayNonUniformIndexing: "
                << (vulkan12.shaderSampledImageArrayNonUniformIndexing ? "true" : "false") << "\n";
            out << "features.runtimeDescriptorArray: "
                << (vulkan12.runtimeDescriptorArray ? "true" : "false") << "\n";
            out << "features.bufferDeviceAddress: "
                << (vulkan12.bufferDeviceAddress ? "true" : "false") << "\n";
            out << "features.timelineSemaphore: "
                << (vulkan12.timelineSemaphore ? "true" : "false") << "\n";
        } else {
            out << "features.descriptorIndexing: unavailable (Vulkan < 1.2)\n";
            out << "features.shaderSampledImageArrayNonUniformIndexing: unavailable (Vulkan < 1.2)\n";
            out << "features.runtimeDescriptorArray: unavailable (Vulkan < 1.2)\n";
            out << "features.bufferDeviceAddress: unavailable (Vulkan < 1.2)\n";
            out << "features.timelineSemaphore: unavailable (Vulkan < 1.2)\n";
        }

        if (has_interlock) {
            out << "features.fragmentShaderInterlock.pixel: "
                << (interlock.fragmentShaderPixelInterlock ? "true" : "false") << "\n";
            out << "features.fragmentShaderInterlock.sample: "
                << (interlock.fragmentShaderSampleInterlock ? "true" : "false") << "\n";
            out << "features.fragmentShaderInterlock.shadingRate: "
                << (interlock.fragmentShaderShadingRateInterlock ? "true" : "false") << "\n";
        } else {
            out << "features.fragmentShaderInterlock: unsupported/not advertised\n";
        }
    } else {
        out << "features.descriptorIndexing: unavailable (features2 query unsupported)\n";
        out << "features.shaderSampledImageArrayNonUniformIndexing: unavailable (features2 query unsupported)\n";
        out << "features.runtimeDescriptorArray: unavailable (features2 query unsupported)\n";
        out << "features.bufferDeviceAddress: unavailable (features2 query unsupported)\n";
        out << "features.timelineSemaphore: unavailable (features2 query unsupported)\n";
        out << "features.fragmentShaderInterlock: unavailable (features2 query unsupported)\n";
    }

    if (has_float_controls && can_query_features2) {
        VkPhysicalDeviceFloatControlsProperties float_controls{};
        float_controls.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FLOAT_CONTROLS_PROPERTIES;

        VkPhysicalDeviceProperties2 properties2{};
        properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        properties2.pNext = &float_controls;
        vkGetPhysicalDeviceProperties2(device, &properties2);

        out << "floatControls.shaderDenormFlushToZeroFloat32: "
            << (float_controls.shaderDenormFlushToZeroFloat32 ? "true" : "false") << "\n";
        out << "floatControls.shaderRoundingModeRTEFloat32: "
            << (float_controls.shaderRoundingModeRTEFloat32 ? "true" : "false") << "\n";
    } else {
        out << "floatControls: unavailable for property query\n";
    }
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_com_saintsm_smoketest_MainActivity_nativeGetVulkanReport(JNIEnv* env, jclass) {
    std::ostringstream out;
    VkApplicationInfo app_info{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app_info.pApplicationName = "SaintsRM Vulkan Report";
    app_info.applicationVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.pEngineName = "SaintsRM";
    app_info.engineVersion = VK_MAKE_VERSION(0, 1, 0);
    app_info.apiVersion = VK_API_VERSION_1_1;

    const char* extensions[] = {VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo create_info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    create_info.pApplicationInfo = &app_info;
    create_info.enabledExtensionCount = 1;
    create_info.ppEnabledExtensionNames = extensions;

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult create_result = vkCreateInstance(&create_info, nullptr, &instance);
    if (create_result != VK_SUCCESS) {
        out << "Vulkan detailed report: FAILED\n";
        out << "vkCreateInstance result: " << create_result << "\n";
        return env->NewStringUTF(out.str().c_str());
    }

    out << "Vulkan detailed report: PASS\n";
    append_instance_extensions(out);

    uint32_t device_count = 0;
    VkResult result = vkEnumeratePhysicalDevices(instance, &device_count, nullptr);
    out << "Vulkan physical devices: " << device_count << "\n";
    if (result == VK_SUCCESS && device_count > 0) {
        std::vector<VkPhysicalDevice> devices(device_count);
        result = vkEnumeratePhysicalDevices(instance, &device_count, devices.data());
        if (result == VK_SUCCESS) {
            append_device_report(out, devices[0]);
        } else {
            out << "physical-device enumeration failed: " << result << "\n";
        }
    }

    vkDestroyInstance(instance, nullptr);
    return env->NewStringUTF(out.str().c_str());
}
