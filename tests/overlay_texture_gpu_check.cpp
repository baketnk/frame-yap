// Explicit offscreen Vulkan check. Not registered with CTest: --run opts into
// GPU access. No OpenVR initialization, desktop surface, microphone or input.
#include "overlay_texture.hpp"
#include <bit>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string_view>

namespace {
void check(VkResult result, const char* operation) {
    if (result != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(result));
}
struct Readback {
    VkDevice device{};
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkCommandPool pool{};
    void* mapped = nullptr;
    ~Readback() {
        if (device) vkDeviceWaitIdle(device);
        if (mapped) vkUnmapMemory(device, memory);
        if (pool) vkDestroyCommandPool(device, pool, nullptr);
        if (buffer) vkDestroyBuffer(device, buffer, nullptr);
        if (memory) vkFreeMemory(device, memory, nullptr);
    }
    void verify(const vr::VRVulkanTextureData_t& texture, std::span<const unsigned char> expected) {
        device = texture.m_pDevice;
        VkBufferCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        info.size = expected.size();
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(device, &info, nullptr, &buffer), "create readback buffer");
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
        VkPhysicalDeviceMemoryProperties types{};
        vkGetPhysicalDeviceMemoryProperties(texture.m_pPhysicalDevice, &types);
        uint32_t type = 0;
        while (type < types.memoryTypeCount && (!(requirements.memoryTypeBits & (1u << type)) ||
               !(types.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) ++type;
        if (type == types.memoryTypeCount) throw std::runtime_error("No host-visible readback memory");
        VkMemoryAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        check(vkAllocateMemory(device, &allocation, nullptr, &memory), "allocate readback memory");
        check(vkBindBufferMemory(device, buffer, memory, 0), "bind readback memory");
        VkCommandPoolCreateInfo pool_info{};
        pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pool_info.queueFamilyIndex = texture.m_nQueueFamilyIndex;
        check(vkCreateCommandPool(device, &pool_info, nullptr, &pool), "create readback command pool");
        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        VkCommandBuffer command{};
        check(vkAllocateCommandBuffers(device, &command_info, &command), "allocate readback commands");
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command, &begin), "begin readback");
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {texture.m_nWidth, texture.m_nHeight, 1};
        vkCmdCopyImageToBuffer(command, std::bit_cast<VkImage>(texture.m_nImage),
                              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
        VkBufferMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer;
        barrier.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, nullptr, 1, &barrier, 0, nullptr);
        check(vkEndCommandBuffer(command), "end readback");
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        check(vkQueueSubmit(texture.m_pQueue, 1, &submit, VK_NULL_HANDLE), "submit readback");
        check(vkQueueWaitIdle(texture.m_pQueue), "wait for readback");
        check(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map readback");
        if (!(types.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = memory;
            range.size = VK_WHOLE_SIZE;
            check(vkInvalidateMappedMemoryRanges(device, 1, &range), "invalidate readback");
        }
        if (std::memcmp(mapped, expected.data(), expected.size()))
            throw std::runtime_error("RGBA readback differs from uploaded pixels");
    }
};
} // namespace

int main(int argc, char** argv) {
    if (argc != 2 || std::string_view(argv[1]) != "--run") {
        std::cout << "Use --run for an offscreen GPU upload/readback check. No OpenVR, audio or input.\n";
        return argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help") ? 0 : 2;
    }
    try {
        frameyap::OverlayTexture texture(1000, 680, {}, [](VkInstance instance) {
            uint32_t count = 0;
            check(vkEnumeratePhysicalDevices(instance, &count, nullptr), "enumerate GPUs");
            if (!count) throw std::runtime_error("No Vulkan GPU");
            std::vector<VkPhysicalDevice> devices(count);
            check(vkEnumeratePhysicalDevices(instance, &count, devices.data()), "enumerate GPUs");
            VkPhysicalDeviceProperties properties{};
            vkGetPhysicalDeviceProperties(devices.front(), &properties);
            std::cout << "Vulkan device: " << properties.deviceName << '\n';
            return devices.front();
        }, [](VkPhysicalDevice) { return std::vector<std::string>{}; });
        std::vector<unsigned char> pixels(1000 * 680 * 4);
        uint64_t image = 0;
        for (unsigned frame = 0; frame < 8; ++frame) {
            for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = (i * 17 + (i / 4000) * 31 + frame * 67) & 255;
            texture.upload(pixels);
            const auto submitted = texture.texture();
            const auto& description = *static_cast<vr::VRVulkanTextureData_t*>(submitted.handle);
            if (frame && description.m_nImage != image) throw std::runtime_error("Texture was recreated");
            image = description.m_nImage;
            Readback readback;
            readback.verify(description, pixels);
        }
        std::cout << "8 exact 1000x680 RGBA readbacks; one persistent Vulkan image.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n'; return 1;
    }
}
