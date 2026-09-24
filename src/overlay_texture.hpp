#pragma once

#include <vulkan/vulkan.h>
#include <openvr.h>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace frameyap {
// One persistent RGBA image, staging allocation and graphics queue. When used
// with OpenVR, construct after initialization; destroy AFTER VR_Shutdown. Upload
// and SetOverlayTexture must run on the same thread (OpenVR uses our queue).
class OverlayTexture {
public:
    using SelectDevice = std::function<VkPhysicalDevice(VkInstance)>;
    using DeviceExtensions = std::function<std::vector<std::string>(VkPhysicalDevice)>;
    OverlayTexture(uint32_t width, uint32_t height,
                   std::span<const std::string> instance_extensions,
                   const SelectDevice& select_device, const DeviceExtensions& device_extensions);
    ~OverlayTexture();
    OverlayTexture(const OverlayTexture&) = delete;
    OverlayTexture& operator=(const OverlayTexture&) = delete;
    void upload(std::span<const unsigned char> rgba);
    vr::Texture_t texture(); // valid after upload; descriptor lives with this object
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace frameyap
