#include "SwapchainManager.h"

SwapchainManager& SwapchainManager::Instance() {
    static SwapchainManager instance;
    return instance;
}