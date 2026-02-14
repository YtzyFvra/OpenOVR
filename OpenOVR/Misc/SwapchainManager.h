#pragma once
#include <mutex>
#include <future>
#include <chrono>
#include <openxr/openxr.h>

class SwapchainManager {
public:
    static SwapchainManager& Instance();
    std::mutex& GetSwapchainMutex() { return swapchainMutex; }
    XrSwapchain& GetSwapchain() { return chain; }
    void SetSwapchain(XrSwapchain swapchain) { chain = swapchain; }

    // Add a helper method for timeout-protected release
    static XrResult ReleaseSwapchainWithTimeout(XrSwapchain swapchain, uint32_t timeoutMs = 100) {
        auto future = std::async(std::launch::async, [swapchain]() {
            XrSwapchainImageReleaseInfo releaseInfo{ XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
            return xrReleaseSwapchainImage(swapchain, &releaseInfo);
        });

        // Wait for completion or timeout
        if (future.wait_for(std::chrono::milliseconds(timeoutMs)) == std::future_status::timeout) {
            OOVR_LOG("Swapchain release timed out!");
            return XR_TIMEOUT_EXPIRED;
        }

        return future.get();
    }

private:
    SwapchainManager() = default;
    ~SwapchainManager() = default;
    std::mutex swapchainMutex;
    XrSwapchain chain = XR_NULL_HANDLE;
};