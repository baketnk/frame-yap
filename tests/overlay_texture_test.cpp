// Link against these Vulkan fakes, never the loader. CTest must not touch a GPU
// or initialize OpenVR. Model queued transfers so reuse before completion fails.
#include "overlay_texture.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>

namespace {
template<class T> T handle(uintptr_t value) { return std::bit_cast<T>(value); }
std::set<uintptr_t> live;
uintptr_t next_handle = 10;
unsigned calls = 0, fail_at = 0, images = 0, submits = 0, flushes = 0;
bool coherent = true;
std::map<VkDeviceMemory, std::vector<unsigned char>> allocations;
VkDeviceMemory image_memory{}, buffer_memory{};
VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
std::vector<std::function<void()>> recorded, pending;
std::vector<std::vector<unsigned char>> observed;
VkResult result() { return ++calls == fail_at ? VK_ERROR_OUT_OF_DEVICE_MEMORY : VK_SUCCESS; }
template<class T> VkResult create(T* out) {
    auto status = result();
    if (status == VK_SUCCESS) { *out = handle<T>(next_handle++); live.insert(std::bit_cast<uintptr_t>(*out)); }
    return status;
}
template<class T> void destroy(T resource) { assert(live.erase(std::bit_cast<uintptr_t>(resource)) == 1); }
void complete() { for (auto& work : pending) work(); pending.clear(); }
void reset() {
    assert(live.empty() && allocations.empty() && pending.empty());
    calls = images = submits = flushes = 0;
    recorded.clear(); observed.clear(); layout = VK_IMAGE_LAYOUT_UNDEFINED;
}
const std::vector<std::string> instance_extensions{"VK_KHR_external_memory_capabilities"};
auto select_device = [](VkInstance instance) {
    assert(live.contains(std::bit_cast<uintptr_t>(instance)));
    return handle<VkPhysicalDevice>(2);
};
auto device_extensions = [](VkPhysicalDevice physical) {
    assert(physical == handle<VkPhysicalDevice>(2));
    return std::vector<std::string>{"VK_KHR_external_memory"};
};
void consume(frameyap::OverlayTexture& texture) {
    const auto t = texture.texture();
    assert(t.eType == vr::TextureType_Vulkan && t.eColorSpace == vr::ColorSpace_Gamma);
    const auto& data = *static_cast<vr::VRVulkanTextureData_t*>(t.handle);
    assert(data.m_nWidth == 2 && data.m_nHeight == 2 && data.m_nSampleCount == 1);
    assert(data.m_nFormat == VK_FORMAT_R8G8B8A8_UNORM && data.m_nQueueFamilyIndex == 1);
    assert(data.m_pPhysicalDevice == handle<VkPhysicalDevice>(2));
    assert(data.m_pQueue == handle<VkQueue>(3));
    assert(live.contains(data.m_nImage));
    // Emulate SetOverlayTexture enqueuing a read AFTER our upload, on our queue.
    pending.push_back([] {
        assert(layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        auto& bytes = allocations.at(image_memory);
        observed.emplace_back(bytes.begin(), bytes.begin() + 16);
    });
}
template<class F> void throws(F f) {
    bool threw = false;
    try { f(); } catch (const std::runtime_error&) { threw = true; }
    assert(threw);
}
} // namespace

extern "C" {
VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo* info, const VkAllocationCallbacks*, VkInstance* out) {
    assert(info->enabledExtensionCount == 1);
    assert(std::string(info->ppEnabledExtensionNames[0]) == instance_extensions[0]);
    return create(out);
}
VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance instance, const VkAllocationCallbacks*) {
    assert(live.size() == 1); destroy(instance);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties* properties) {
    properties->limits.maxImageDimension2D = 4096;
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t* count, VkQueueFamilyProperties* out) {
    if (!out) { *count = 2; return; }
    assert(*count == 2);
    out[0] = {}; out[0].queueFlags = VK_QUEUE_TRANSFER_BIT; out[0].queueCount = 1;
    out[1] = {}; out[1].queueFlags = VK_QUEUE_GRAPHICS_BIT; out[1].queueCount = 1;
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
                                             const VkAllocationCallbacks*, VkDevice* out) {
    assert(physical == handle<VkPhysicalDevice>(2));
    assert(info->queueCreateInfoCount == 1 && info->pQueueCreateInfos->queueFamilyIndex == 1);
    assert(info->enabledExtensionCount == 1);
    assert(std::string(info->ppEnabledExtensionNames[0]) == "VK_KHR_external_memory");
    return create(out);
}
VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice device, const VkAllocationCallbacks*) {
    assert(live.size() == 2 && allocations.empty() && pending.empty()); destroy(device);
}
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice) { complete(); return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice, uint32_t family, uint32_t index, VkQueue* queue) {
    assert(family == 1 && index == 0); *queue = handle<VkQueue>(3);
}
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* memory) {
    memory->memoryTypeCount = 2;
    memory->memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    memory->memoryTypes[1].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        (coherent ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0);
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo* info, const VkAllocationCallbacks*, VkImage* out) {
    assert(info->format == VK_FORMAT_R8G8B8A8_UNORM);
    assert(info->extent.width == 2 && info->extent.height == 2 && info->extent.depth == 1);
    assert(info->mipLevels == 1 && info->arrayLayers == 1 && info->samples == VK_SAMPLE_COUNT_1_BIT);
    assert(info->tiling == VK_IMAGE_TILING_OPTIMAL);
    assert(info->usage == (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT));
    ++images; return create(out);
}
VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice, VkImage image, const VkAllocationCallbacks*) { destroy(image); }
VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements* requirements) {
    *requirements = {256, 256, 1};
}
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo* info, const VkAllocationCallbacks*, VkDeviceMemory* out) {
    auto status = create(out);
    if (status == VK_SUCCESS) allocations[*out].resize(info->allocationSize);
    return status;
}
VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks*) {
    assert(allocations.erase(memory) == 1); destroy(memory);
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory memory, VkDeviceSize) {
    image_memory = memory; return result();
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo* info, const VkAllocationCallbacks*, VkBuffer* out) {
    assert(info->size == 16 && info->usage == VK_BUFFER_USAGE_TRANSFER_SRC_BIT); return create(out);
}
VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks*) { destroy(buffer); }
VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements* requirements) {
    *requirements = {256, 256, 2};
}
VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory memory, VkDeviceSize) {
    buffer_memory = memory; return result();
}
VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize size,
                                         VkMemoryMapFlags, void** out) {
    assert(offset == 0 && size == VK_WHOLE_SIZE);
    auto status = result();
    if (status == VK_SUCCESS) *out = allocations.at(memory).data();
    return status;
}
VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory memory) { assert(allocations.contains(memory)); }
VKAPI_ATTR VkResult VKAPI_CALL vkFlushMappedMemoryRanges(VkDevice, uint32_t count, const VkMappedMemoryRange* range) {
    assert(!coherent && count == 1 && range->offset == 0 && range->size == VK_WHOLE_SIZE);
    assert(range->memory == buffer_memory); ++flushes; return result();
}
VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo* info, const VkAllocationCallbacks*, VkCommandPool* out) {
    assert(info->queueFamilyIndex == 1); return create(out);
}
VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice, VkCommandPool pool, const VkAllocationCallbacks*) { destroy(pool); }
VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*, VkCommandBuffer* out) {
    auto status = result(); if (status == VK_SUCCESS) *out = handle<VkCommandBuffer>(4); return status;
}
VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue queue) {
    assert(queue == handle<VkQueue>(3)); auto status = result(); if (status == VK_SUCCESS) complete(); return status;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags) {
    assert(pending.empty()); recorded.clear(); return result();
}
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*) { return result(); }
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
    VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t, const VkBufferMemoryBarrier*,
    uint32_t count, const VkImageMemoryBarrier* barriers) {
    assert(count == 1);
    const auto b = barriers[0];
    assert(b.srcQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED && b.dstQueueFamilyIndex == VK_QUEUE_FAMILY_IGNORED);
    assert(b.subresourceRange.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT);
    assert(b.subresourceRange.levelCount == 1 && b.subresourceRange.layerCount == 1);
    recorded.push_back([b] { assert(layout == b.oldLayout); layout = b.newLayout; });
}
VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage, VkImageLayout target,
                                                uint32_t count, const VkBufferImageCopy* copy) {
    assert(count == 1 && target == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    assert(copy->bufferRowLength == 0 && copy->bufferImageHeight == 0 && copy->bufferOffset == 0);
    assert(copy->imageExtent.width == 2 && copy->imageExtent.height == 2 && copy->imageExtent.depth == 1);
    recorded.push_back([] {
        assert(layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        std::copy_n(allocations.at(buffer_memory).begin(), 16, allocations.at(image_memory).begin());
    });
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer) { return result(); }
VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count, const VkSubmitInfo* submit, VkFence) {
    assert(queue == handle<VkQueue>(3) && count == 1 && submit->commandBufferCount == 1);
    auto status = result();
    if (status == VK_SUCCESS) { pending.insert(pending.end(), recorded.begin(), recorded.end()); ++submits; }
    return status;
}
} // extern C

int main() {
    const std::array<unsigned char, 16> first{255,0,0,255, 0,255,0,128, 0,0,255,0, 255,255,255,255};
    const std::array<unsigned char, 16> second{0,0,0,0, 4,5,6,7, 8,9,10,11, 12,13,14,15};
    for (bool use_coherent : {true, false}) {
        reset(); coherent = use_coherent;
        {
            frameyap::OverlayTexture texture(2, 2, instance_extensions, select_device, device_extensions);
            throws([&] { texture.texture(); });
            throws([&] { texture.upload(std::span(first).first(15)); });
            texture.upload(first);
            const auto descriptor = texture.texture();
            const auto image = static_cast<vr::VRVulkanTextureData_t*>(descriptor.handle)->m_nImage;
            consume(texture);
            texture.upload(second); // must drain first read before replacing staging bytes
            assert(observed.size() == 1 && std::ranges::equal(observed[0], first));
            assert(texture.texture().handle == descriptor.handle);
            assert(static_cast<vr::VRVulkanTextureData_t*>(descriptor.handle)->m_nImage == image);
            consume(texture);
            assert(images == 1 && submits == 2 && flushes == (coherent ? 0u : 2u));
        }
        assert(observed.size() == 2 && std::ranges::equal(observed[1], second));
        assert(live.empty() && allocations.empty());
        const auto operation_count = calls;
        // Every fallible allocation/upload operation must unwind all owned
        // resources, including a failure while a previous frame is pending.
        for (unsigned failure = 1; failure <= operation_count; ++failure) {
            reset(); fail_at = failure;
            throws([&] {
                frameyap::OverlayTexture texture(2, 2, instance_extensions, select_device, device_extensions);
                texture.upload(first); consume(texture);
                texture.upload(second); consume(texture);
            });
            assert(live.empty() && allocations.empty() && pending.empty());
        }
        fail_at = 0;
    }
    reset();
    throws([&] { frameyap::OverlayTexture texture(0, 2, instance_extensions, select_device, device_extensions); });
    assert(calls == 0); // invalid sizes must not initialize Vulkan
    throws([&] {
        frameyap::OverlayTexture texture(2, 2, instance_extensions,
            [](VkInstance) { return VkPhysicalDevice{}; }, device_extensions);
    });
    assert(live.empty()); // never pick an unrelated GPU when SteamVR returns none
}
