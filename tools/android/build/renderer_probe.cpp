// Exercise the port's complete Vulkan device setup on the Android FEX runtime.
#include "video_core/renderer_vulkan/vk_instance.h"
#include <cstdio>
#include <exception>

int main() {
    try {
        Vulkan::Instance instance(-1, false);
        // Keep Vulkan-Hpp dispatch inside libgow3gpu, where it is initialized.
        std::printf("PASS: port renderer device created; driver %s\n",
                    instance.GetDriverVersionName().c_str());
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "Renderer device setup failed: %s\n", error.what());
        return 1;
    }
}
