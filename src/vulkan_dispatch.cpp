#include "vulkan_dispatch.h"
#include <dlfcn.h>
PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
PFN_vkAllocateMemory vkAllocateMemory = nullptr;
PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
PFN_vkBindImageMemory vkBindImageMemory = nullptr;
PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
PFN_vkCreateBuffer vkCreateBuffer = nullptr;
PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
PFN_vkCreateDevice vkCreateDevice = nullptr;
PFN_vkCreateFence vkCreateFence = nullptr;
PFN_vkCreateImage vkCreateImage = nullptr;
PFN_vkCreateInstance vkCreateInstance = nullptr;
PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
PFN_vkDestroyDevice vkDestroyDevice = nullptr;
PFN_vkDestroyFence vkDestroyFence = nullptr;
PFN_vkDestroyImage vkDestroyImage = nullptr;
PFN_vkDestroyInstance vkDestroyInstance = nullptr;
PFN_vkDeviceWaitIdle vkDeviceWaitIdle = nullptr;
PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
PFN_vkEnumerateDeviceExtensionProperties vkEnumerateDeviceExtensionProperties = nullptr;
PFN_vkEnumerateInstanceExtensionProperties vkEnumerateInstanceExtensionProperties = nullptr;
PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices = nullptr;
PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
PFN_vkFreeMemory vkFreeMemory = nullptr;
PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkGetPhysicalDeviceProperties vkGetPhysicalDeviceProperties = nullptr;
PFN_vkGetPhysicalDeviceQueueFamilyProperties vkGetPhysicalDeviceQueueFamilyProperties = nullptr;
PFN_vkMapMemory vkMapMemory = nullptr;
PFN_vkQueueSubmit vkQueueSubmit = nullptr;
PFN_vkResetCommandBuffer vkResetCommandBuffer = nullptr;
PFN_vkResetFences vkResetFences = nullptr;
PFN_vkUnmapMemory vkUnmapMemory = nullptr;
PFN_vkWaitForFences vkWaitForFences = nullptr;
bool loadVulkan(std::string &error) {
    static void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!library) {
        error = std::string("Vulkan loader: ") + dlerror();
        return false;
    }
    vkAllocateCommandBuffers =
        reinterpret_cast<PFN_vkAllocateCommandBuffers>(dlsym(library, "vkAllocateCommandBuffers"));
    if (!vkAllocateCommandBuffers) {
        error = "Missing Vulkan function: vkAllocateCommandBuffers";
        return false;
    }
    vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(dlsym(library, "vkAllocateMemory"));
    if (!vkAllocateMemory) {
        error = "Missing Vulkan function: vkAllocateMemory";
        return false;
    }
    vkBeginCommandBuffer =
        reinterpret_cast<PFN_vkBeginCommandBuffer>(dlsym(library, "vkBeginCommandBuffer"));
    if (!vkBeginCommandBuffer) {
        error = "Missing Vulkan function: vkBeginCommandBuffer";
        return false;
    }
    vkBindBufferMemory =
        reinterpret_cast<PFN_vkBindBufferMemory>(dlsym(library, "vkBindBufferMemory"));
    if (!vkBindBufferMemory) {
        error = "Missing Vulkan function: vkBindBufferMemory";
        return false;
    }
    vkBindImageMemory =
        reinterpret_cast<PFN_vkBindImageMemory>(dlsym(library, "vkBindImageMemory"));
    if (!vkBindImageMemory) {
        error = "Missing Vulkan function: vkBindImageMemory";
        return false;
    }
    vkCmdCopyBufferToImage =
        reinterpret_cast<PFN_vkCmdCopyBufferToImage>(dlsym(library, "vkCmdCopyBufferToImage"));
    if (!vkCmdCopyBufferToImage) {
        error = "Missing Vulkan function: vkCmdCopyBufferToImage";
        return false;
    }
    vkCmdPipelineBarrier =
        reinterpret_cast<PFN_vkCmdPipelineBarrier>(dlsym(library, "vkCmdPipelineBarrier"));
    if (!vkCmdPipelineBarrier) {
        error = "Missing Vulkan function: vkCmdPipelineBarrier";
        return false;
    }
    vkCreateBuffer = reinterpret_cast<PFN_vkCreateBuffer>(dlsym(library, "vkCreateBuffer"));
    if (!vkCreateBuffer) {
        error = "Missing Vulkan function: vkCreateBuffer";
        return false;
    }
    vkCreateCommandPool =
        reinterpret_cast<PFN_vkCreateCommandPool>(dlsym(library, "vkCreateCommandPool"));
    if (!vkCreateCommandPool) {
        error = "Missing Vulkan function: vkCreateCommandPool";
        return false;
    }
    vkCreateDevice = reinterpret_cast<PFN_vkCreateDevice>(dlsym(library, "vkCreateDevice"));
    if (!vkCreateDevice) {
        error = "Missing Vulkan function: vkCreateDevice";
        return false;
    }
    vkCreateFence = reinterpret_cast<PFN_vkCreateFence>(dlsym(library, "vkCreateFence"));
    if (!vkCreateFence) {
        error = "Missing Vulkan function: vkCreateFence";
        return false;
    }
    vkCreateImage = reinterpret_cast<PFN_vkCreateImage>(dlsym(library, "vkCreateImage"));
    if (!vkCreateImage) {
        error = "Missing Vulkan function: vkCreateImage";
        return false;
    }
    vkCreateInstance = reinterpret_cast<PFN_vkCreateInstance>(dlsym(library, "vkCreateInstance"));
    if (!vkCreateInstance) {
        error = "Missing Vulkan function: vkCreateInstance";
        return false;
    }
    vkDestroyBuffer = reinterpret_cast<PFN_vkDestroyBuffer>(dlsym(library, "vkDestroyBuffer"));
    if (!vkDestroyBuffer) {
        error = "Missing Vulkan function: vkDestroyBuffer";
        return false;
    }
    vkDestroyCommandPool =
        reinterpret_cast<PFN_vkDestroyCommandPool>(dlsym(library, "vkDestroyCommandPool"));
    if (!vkDestroyCommandPool) {
        error = "Missing Vulkan function: vkDestroyCommandPool";
        return false;
    }
    vkDestroyDevice = reinterpret_cast<PFN_vkDestroyDevice>(dlsym(library, "vkDestroyDevice"));
    if (!vkDestroyDevice) {
        error = "Missing Vulkan function: vkDestroyDevice";
        return false;
    }
    vkDestroyFence = reinterpret_cast<PFN_vkDestroyFence>(dlsym(library, "vkDestroyFence"));
    if (!vkDestroyFence) {
        error = "Missing Vulkan function: vkDestroyFence";
        return false;
    }
    vkDestroyImage = reinterpret_cast<PFN_vkDestroyImage>(dlsym(library, "vkDestroyImage"));
    if (!vkDestroyImage) {
        error = "Missing Vulkan function: vkDestroyImage";
        return false;
    }
    vkDestroyInstance =
        reinterpret_cast<PFN_vkDestroyInstance>(dlsym(library, "vkDestroyInstance"));
    if (!vkDestroyInstance) {
        error = "Missing Vulkan function: vkDestroyInstance";
        return false;
    }
    vkDeviceWaitIdle = reinterpret_cast<PFN_vkDeviceWaitIdle>(dlsym(library, "vkDeviceWaitIdle"));
    if (!vkDeviceWaitIdle) {
        error = "Missing Vulkan function: vkDeviceWaitIdle";
        return false;
    }
    vkEndCommandBuffer =
        reinterpret_cast<PFN_vkEndCommandBuffer>(dlsym(library, "vkEndCommandBuffer"));
    if (!vkEndCommandBuffer) {
        error = "Missing Vulkan function: vkEndCommandBuffer";
        return false;
    }
    vkEnumerateDeviceExtensionProperties =
        reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
            dlsym(library, "vkEnumerateDeviceExtensionProperties"));
    if (!vkEnumerateDeviceExtensionProperties) {
        error = "Missing Vulkan function: vkEnumerateDeviceExtensionProperties";
        return false;
    }
    vkEnumerateInstanceExtensionProperties =
        reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
            dlsym(library, "vkEnumerateInstanceExtensionProperties"));
    if (!vkEnumerateInstanceExtensionProperties) {
        error = "Missing Vulkan function: vkEnumerateInstanceExtensionProperties";
        return false;
    }
    vkEnumeratePhysicalDevices = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
        dlsym(library, "vkEnumeratePhysicalDevices"));
    if (!vkEnumeratePhysicalDevices) {
        error = "Missing Vulkan function: vkEnumeratePhysicalDevices";
        return false;
    }
    vkFreeCommandBuffers =
        reinterpret_cast<PFN_vkFreeCommandBuffers>(dlsym(library, "vkFreeCommandBuffers"));
    if (!vkFreeCommandBuffers) {
        error = "Missing Vulkan function: vkFreeCommandBuffers";
        return false;
    }
    vkFreeMemory = reinterpret_cast<PFN_vkFreeMemory>(dlsym(library, "vkFreeMemory"));
    if (!vkFreeMemory) {
        error = "Missing Vulkan function: vkFreeMemory";
        return false;
    }
    vkGetBufferMemoryRequirements = reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
        dlsym(library, "vkGetBufferMemoryRequirements"));
    if (!vkGetBufferMemoryRequirements) {
        error = "Missing Vulkan function: vkGetBufferMemoryRequirements";
        return false;
    }
    vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(dlsym(library, "vkGetDeviceQueue"));
    if (!vkGetDeviceQueue) {
        error = "Missing Vulkan function: vkGetDeviceQueue";
        return false;
    }
    vkGetImageMemoryRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
        dlsym(library, "vkGetImageMemoryRequirements"));
    if (!vkGetImageMemoryRequirements) {
        error = "Missing Vulkan function: vkGetImageMemoryRequirements";
        return false;
    }
    vkGetPhysicalDeviceMemoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(
        dlsym(library, "vkGetPhysicalDeviceMemoryProperties"));
    if (!vkGetPhysicalDeviceMemoryProperties) {
        error = "Missing Vulkan function: vkGetPhysicalDeviceMemoryProperties";
        return false;
    }
    vkGetPhysicalDeviceProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
        dlsym(library, "vkGetPhysicalDeviceProperties"));
    if (!vkGetPhysicalDeviceProperties) {
        error = "Missing Vulkan function: vkGetPhysicalDeviceProperties";
        return false;
    }
    vkGetPhysicalDeviceQueueFamilyProperties =
        reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            dlsym(library, "vkGetPhysicalDeviceQueueFamilyProperties"));
    if (!vkGetPhysicalDeviceQueueFamilyProperties) {
        error = "Missing Vulkan function: vkGetPhysicalDeviceQueueFamilyProperties";
        return false;
    }
    vkMapMemory = reinterpret_cast<PFN_vkMapMemory>(dlsym(library, "vkMapMemory"));
    if (!vkMapMemory) {
        error = "Missing Vulkan function: vkMapMemory";
        return false;
    }
    vkQueueSubmit = reinterpret_cast<PFN_vkQueueSubmit>(dlsym(library, "vkQueueSubmit"));
    if (!vkQueueSubmit) {
        error = "Missing Vulkan function: vkQueueSubmit";
        return false;
    }
    vkResetCommandBuffer =
        reinterpret_cast<PFN_vkResetCommandBuffer>(dlsym(library, "vkResetCommandBuffer"));
    if (!vkResetCommandBuffer) {
        error = "Missing Vulkan function: vkResetCommandBuffer";
        return false;
    }
    vkResetFences = reinterpret_cast<PFN_vkResetFences>(dlsym(library, "vkResetFences"));
    if (!vkResetFences) {
        error = "Missing Vulkan function: vkResetFences";
        return false;
    }
    vkUnmapMemory = reinterpret_cast<PFN_vkUnmapMemory>(dlsym(library, "vkUnmapMemory"));
    if (!vkUnmapMemory) {
        error = "Missing Vulkan function: vkUnmapMemory";
        return false;
    }
    vkWaitForFences = reinterpret_cast<PFN_vkWaitForFences>(dlsym(library, "vkWaitForFences"));
    if (!vkWaitForFences) {
        error = "Missing Vulkan function: vkWaitForFences";
        return false;
    }
    return true;
}
