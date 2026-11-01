// SPDX-License-Identifier: GPL-3.0-or-later
// A Vulkan game's calls, as a Switch game makes them (VK_NN_vi_surface for its window), bridged to the PC's driver
// (gfx/hle_vk.c and the generated bridges): the instance with the Switch's surface extension, the GPU, a device, memory
// the CPU writes and the GPU fills, a submission and its fence, vkGetInstanceProcAddr's addresses, a debug messenger
// whose guest callback must not be called, and the window surface with a swapchain presented to. The tests run it on
// lavapipe (headless: the surface is a headless one).
#include "nx.h"
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_VI_NN 1
#include <vulkan/vulkan.h>
// the imports, as the game links them (from its Vulkan loader)
VkResult vkCreateInstance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *);
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance, const char *);
VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t *, VkPhysicalDevice *);
void vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *);
void vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
VkResult vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *);
void vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);
VkResult vkCreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *);
void vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *);
VkResult vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *);
VkResult vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
VkResult vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **);
VkResult vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *, VkCommandPool *);
VkResult vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
VkResult vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
void vkCmdFillBuffer(VkCommandBuffer, VkBuffer, VkDeviceSize, VkDeviceSize, uint32_t);
VkResult vkEndCommandBuffer(VkCommandBuffer);
VkResult vkCreateFence(VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *);
VkResult vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
VkResult vkWaitForFences(VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t);
VkResult vkResetFences(VkDevice, uint32_t, const VkFence *);
VkResult vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags);
VkResult vkCreateViSurfaceNN(VkInstance, const VkViSurfaceCreateInfoNN *, const VkAllocationCallbacks *, VkSurfaceKHR *);
VkResult vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR, VkBool32 *);
VkResult vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, VkSurfaceCapabilitiesKHR *);
VkResult vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *, VkSurfaceFormatKHR *);
VkResult vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *, VkSwapchainKHR *);
VkResult vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
VkResult vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *);
void vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags, uint32_t, const VkMemoryBarrier *,
                          uint32_t, const VkBufferMemoryBarrier *, uint32_t, const VkImageMemoryBarrier *);
void vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t, const VkImageSubresourceRange *);
VkResult vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR *);
VkResult vkEnumerateInstanceExtensionProperties(const char *, uint32_t *, VkExtensionProperties *);

static int g_called;
static VkBool32 guest_cb(VkDebugUtilsMessageSeverityFlagBitsEXT s, VkDebugUtilsMessageTypeFlagsEXT t, const VkDebugUtilsMessengerCallbackDataEXT *d, void *u) {
    g_called = 1;
    return VK_FALSE;
}
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }
static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                              from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

int test_main(void) {
    uint32_t ne = 0;
    static VkExtensionProperties props[256];
    vkEnumerateInstanceExtensionProperties(NULL, &ne, NULL);
    if (ne > 256) ne = 256;
    vkEnumerateInstanceExtensionProperties(NULL, &ne, props);
    int vi = 0;
    for (uint32_t i = 0; i < ne; i++) vi |= streq(props[i].extensionName, VK_NN_VI_SURFACE_EXTENSION_NAME);
    CHECK("the instance extensions include the Switch's window surface", vi);

    VkDebugUtilsMessengerCreateInfoEXT dbg = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0, 0x1111, 0x7, guest_cb, NULL};
    const char *ext[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "t_vk", 1, "nxl-test", 1, VK_API_VERSION_1_1};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &dbg, 0, &app, 0, NULL, 2, ext};
    VkInstance inst = NULL;
    CHECK("vkCreateInstance with VK_NN_vi_surface", vkCreateInstance(&ici, NULL, &inst) == VK_SUCCESS && inst);
    PFN_vkEnumeratePhysicalDevices enum_pd = (PFN_vkEnumeratePhysicalDevices)vkGetInstanceProcAddr(inst, "vkEnumeratePhysicalDevices");
    CHECK("vkGetInstanceProcAddr: an address for vkEnumeratePhysicalDevices", enum_pd != NULL);
    CHECK("... none for a name the driver lacks", vkGetInstanceProcAddr(inst, "vkNotARealFunctionNXL") == NULL);
    PFN_vkCreateDebugUtilsMessengerEXT mk = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerEXT msgr = 0;
    dbg.pNext = NULL;
    CHECK("a debug messenger with the game's callback", !mk || mk(inst, &dbg, NULL, &msgr) == VK_SUCCESS);
    uint32_t npd = 1;
    VkPhysicalDevice pd = NULL;
    VkResult er = enum_pd ? enum_pd(inst, &npd, &pd) : VK_ERROR_INITIALIZATION_FAILED;
    CHECK("a GPU (through the address vkGetInstanceProcAddr gave)", (er == VK_SUCCESS || er == VK_INCOMPLETE) && pd);
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(pd, &pp);
    print("# %s\n", pp.deviceName);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio};
    const char *dext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, "VK_NV_not_on_this_gpu"};
    VkPhysicalDeviceFeatures feat = {0};
    feat.shaderFloat64 = VK_TRUE;   // asked for whether or not the GPU has it
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 2, dext, &feat};
    VkDevice dev = NULL;
    CHECK("vkCreateDevice (an extension the GPU lacks left out)", vkCreateDevice(pd, &dci, NULL, &dev) == VK_SUCCESS && dev);
    VkQueue q = NULL;
    vkGetDeviceQueue(dev, 0, 0, &q);

    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE};
    VkBuffer buf = 0;
    vkCreateBuffer(dev, &bci, NULL, &buf);
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t mt = 0;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits >> i & 1) && (mp.memoryTypes[i].propertyFlags & 6) == 6) { mt = i; break; }
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
    VkDeviceMemory mem = 0;
    u32 *p = NULL;
    CHECK("host-visible memory, bound and mapped", vkAllocateMemory(dev, &mai, NULL, &mem) == VK_SUCCESS && vkBindBufferMemory(dev, buf, mem, 0) == VK_SUCCESS &&
                                                   vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&p) == VK_SUCCESS && p);
    if (p) p[0] = 1, p[1023] = 2;
    VkCommandPoolCreateInfo cpi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0};
    VkCommandPool pool = 0;
    vkCreateCommandPool(dev, &cpi, NULL, &pool);
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    VkCommandBuffer cb = NULL;
    vkAllocateCommandBuffers(dev, &cai, &cb);
    VkCommandBufferBeginInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cb, &cbi);
    vkCmdFillBuffer(cb, buf, 0, 4096, 0xC0FFEE11u);
    vkEndCommandBuffer(cb);
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = 0;
    vkCreateFence(dev, &fci, NULL, &fence);
    VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb};
    CHECK("a submission and its fence", vkQueueSubmit(q, 1, &si, fence) == VK_SUCCESS && vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS);
    CHECK("... the GPU filled the memory the game reads", p && p[0] == 0xC0FFEE11u && p[1023] == 0xC0FFEE11u);

    VkViSurfaceCreateInfoNN vci = {VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, NULL, 0, (void *)0x1234};
    VkSurfaceKHR surf = 0;
    CHECK("vkCreateViSurfaceNN: the window's surface", vkCreateViSurfaceNN(inst, &vci, NULL, &surf) == VK_SUCCESS && surf);
    VkBool32 ok = 0;
    vkGetPhysicalDeviceSurfaceSupportKHR(pd, 0, surf, &ok);
    CHECK("... the queue presents to it", ok);
    VkSurfaceCapabilitiesKHR cap;
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &cap);
    VkSurfaceFormatKHR sf;
    uint32_t nf = 1;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, &sf);
    VkSwapchainCreateInfoKHR sci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surf, cap.minImageCount, sf.format, sf.colorSpace, {64, 64}, 1,
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
                                    VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_PRESENT_MODE_FIFO_KHR, VK_TRUE, 0};
    VkSwapchainKHR sc = 0;
    CHECK("a 64x64 swapchain", vkCreateSwapchainKHR(dev, &sci, NULL, &sc) == VK_SUCCESS && sc);
    VkImage imgs[8];
    uint32_t ni = 8, idx = 0;
    vkGetSwapchainImagesKHR(dev, sc, &ni, imgs);
    vkResetFences(dev, 1, &fence);
    int presented = 1;
    for (int f = 0; f < 3 && presented; f++) {
        presented = vkAcquireNextImageKHR(dev, sc, 5000000000ull, 0, fence, &idx) == VK_SUCCESS && vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
        vkResetFences(dev, 1, &fence);
        vkResetCommandBuffer(cb, 0);
        vkBeginCommandBuffer(cb, &cbi);
        barrier(cb, imgs[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkClearColorValue c = {{0.0f, 0.5f, 1.0f, 1.0f}};
        VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rr);
        barrier(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        vkEndCommandBuffer(cb);
        presented &= vkQueueSubmit(q, 1, &si, fence) == VK_SUCCESS && vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
        vkResetFences(dev, 1, &fence);
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &sc, &idx, NULL};
        presented &= vkQueuePresentKHR(q, &pi) == VK_SUCCESS;
    }
    CHECK("three frames cleared and presented", presented);
    CHECK("the game's debug callback was not called by the driver", !g_called);
    return 0;
}
