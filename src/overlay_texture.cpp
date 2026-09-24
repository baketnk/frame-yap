#include "overlay_texture.hpp"

#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace frameyap {
namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string("Overlay Vulkan ") + operation + ": VkResult=" +
                                 std::to_string(result));
}
std::vector<const char*> names(std::span<const std::string> extensions) {
    std::vector<const char*> result;
    for (const auto& extension : extensions) result.push_back(extension.c_str());
    return result;
}
constexpr VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkImageSubresourceRange color_range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
} // namespace

struct OverlayTexture::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory image_memory = VK_NULL_HANDLE, staging_memory = VK_NULL_HANDLE;
    VkBuffer staging = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    vr::VRVulkanTextureData_t description{};
    void* mapped = nullptr;
    size_t bytes = 0;
    uint32_t width = 0, height = 0;
    bool coherent = false, uploaded = false;

    ~Impl() {
        // OpenVR must already have released its client-side Vulkan resources.
        // On device loss, waiting can fail; owned handles still need destruction.
        if (device) vkDeviceWaitIdle(device);
        if (mapped) vkUnmapMemory(device, staging_memory);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (staging) vkDestroyBuffer(device, staging, nullptr);
        if (staging_memory) vkFreeMemory(device, staging_memory, nullptr);
        if (image) vkDestroyImage(device, image, nullptr);
        if (image_memory) vkFreeMemory(device, image_memory, nullptr);
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
    }

    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags required,
                         VkMemoryPropertyFlags preferred) const {
        for (auto flags : {required | preferred, required})
            for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
                    return i;
        throw std::runtime_error("Overlay Vulkan: no compatible memory type");
    }

    void init(uint32_t w, uint32_t h, std::span<const std::string> instance_extensions,
              const SelectDevice& select_device, const DeviceExtensions& device_extensions) {
        if (!w || !h || uint64_t(w) * h > std::numeric_limits<size_t>::max() / 4)
            throw std::runtime_error("Overlay Vulkan: invalid RGBA dimensions");
        width = w; height = h; bytes = size_t(w) * h * 4;
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = "FrameYap";
        app.apiVersion = VK_API_VERSION_1_0;
        auto instance_names = names(instance_extensions);
        VkInstanceCreateInfo instance_info{};
        instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instance_info.pApplicationInfo = &app;
        instance_info.enabledExtensionCount = uint32_t(instance_names.size());
        instance_info.ppEnabledExtensionNames = instance_names.data();
        check(vkCreateInstance(&instance_info, nullptr, &instance), "vkCreateInstance (OpenVR extensions)");
        physical = select_device(instance);
        if (!physical) throw std::runtime_error("Overlay Vulkan: SteamVR did not select a physical device");
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical, &properties);
        if (w > properties.limits.maxImageDimension2D || h > properties.limits.maxImageDimension2D)
            throw std::runtime_error("Overlay Vulkan: panel exceeds device image dimensions");

        uint32_t family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        vkGetPhysicalDeviceQueueFamilyProperties(physical, &family_count, families.data());
        uint32_t family = 0;
        while (family < family_count &&
               (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) || !families[family].queueCount)) ++family;
        if (family == family_count) throw std::runtime_error("Overlay Vulkan: no graphics queue");
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue_info{};
        queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue_info.queueFamilyIndex = family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &priority;
        const auto extensions = device_extensions(physical);
        auto device_names = names(extensions);
        VkDeviceCreateInfo device_info{};
        device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.enabledExtensionCount = uint32_t(device_names.size());
        device_info.ppEnabledExtensionNames = device_names.data();
        check(vkCreateDevice(physical, &device_info, nullptr, &device), "vkCreateDevice (OpenVR extensions)");
        vkGetDeviceQueue(device, family, 0, &queue);
        vkGetPhysicalDeviceMemoryProperties(physical, &memory);

        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = format;
        image_info.extent = {width, height, 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        check(vkCreateImage(device, &image_info, nullptr, &image), "vkCreateImage");
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device, image, &requirements);
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
        check(vkAllocateMemory(device, &allocation, nullptr, &image_memory), "vkAllocateMemory (image)");
        check(vkBindImageMemory(device, image, image_memory, 0), "vkBindImageMemory");

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = bytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &buffer_info, nullptr, &staging), "vkCreateBuffer");
        vkGetBufferMemoryRequirements(device, staging, &requirements);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        coherent = memory.memoryTypes[allocation.memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        check(vkAllocateMemory(device, &allocation, nullptr, &staging_memory), "vkAllocateMemory (staging)");
        check(vkBindBufferMemory(device, staging, staging_memory, 0), "vkBindBufferMemory");
        // Map the allocation, including any padding, so whole-allocation flushes
        // satisfy nonCoherentAtomSize even when the last pixel is unaligned.
        check(vkMapMemory(device, staging_memory, 0, VK_WHOLE_SIZE, 0, &mapped), "vkMapMemory");
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = family;
        pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "vkCreateCommandPool");
        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(device, &command_info, &commands), "vkAllocateCommandBuffers");

        description.m_nImage = std::bit_cast<uint64_t>(image);
        description.m_pDevice = device;
        description.m_pPhysicalDevice = physical;
        description.m_pInstance = instance;
        description.m_pQueue = queue;
        description.m_nQueueFamilyIndex = family;
        description.m_nWidth = width;
        description.m_nHeight = height;
        description.m_nFormat = format;
        description.m_nSampleCount = 1;
    }

    void upload(std::span<const unsigned char> rgba) {
        if (rgba.size() != bytes) throw std::runtime_error("Overlay Vulkan: wrong RGBA upload size");
        // Includes work enqueued by the previous SetOverlayTexture call, not
        // just our upload. Reuse the staging bytes and commands only after it
        // finishes. This dedicated queue is idle between content changes.
        check(vkQueueWaitIdle(queue), "vkQueueWaitIdle");
        std::memcpy(mapped, rgba.data(), bytes);
        if (!coherent) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = staging_memory;
            range.size = VK_WHOLE_SIZE;
            check(vkFlushMappedMemoryRanges(device, 1, &range), "vkFlushMappedMemoryRanges");
        }
        check(vkResetCommandBuffer(commands, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(commands, &begin), "vkBeginCommandBuffer");
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = uploaded ? VK_ACCESS_TRANSFER_READ_BIT : 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.oldLayout = uploaded ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = color_range;
        vkCmdPipelineBarrier(commands, uploaded ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(commands, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &barrier);
        check(vkEndCommandBuffer(commands), "vkEndCommandBuffer");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commands;
        check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE), "vkQueueSubmit");
        // SteamVR enqueues its transfer on this same queue after this upload,
        // with the image in the layout required by OpenVR's Vulkan contract.
        uploaded = true;
    }
};

OverlayTexture::OverlayTexture(uint32_t width, uint32_t height,
                               std::span<const std::string> instance_extensions,
                               const SelectDevice& select_device, const DeviceExtensions& device_extensions)
    : impl_(std::make_unique<Impl>()) {
    impl_->init(width, height, instance_extensions, select_device, device_extensions);
}
OverlayTexture::~OverlayTexture() = default;
void OverlayTexture::upload(std::span<const unsigned char> rgba) { impl_->upload(rgba); }
vr::Texture_t OverlayTexture::texture() {
    if (!impl_->uploaded) throw std::runtime_error("Overlay Vulkan: texture has not been uploaded");
    return {&impl_->description, vr::TextureType_Vulkan, vr::ColorSpace_Gamma};
}
} // namespace frameyap
