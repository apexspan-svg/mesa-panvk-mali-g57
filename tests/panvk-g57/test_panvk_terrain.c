#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <time.h>
#include <xcb/xcb.h>
#define VK_USE_PLATFORM_XCB_KHR
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#define WIN_W 800
#define WIN_H 600
#define GRID_SIZE 64
#define NUM_FRAMES 300

#define CHECK(x, msg) do { \
    VkResult r = (x); \
    if (r != VK_SUCCESS) { \
        printf("FAILED %s: VkResult=%d\n", msg, r); \
        return 1; \
    } \
} while (0)

static double get_time_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int load_file(const char *path, uint8_t *buf, size_t buf_cap, size_t *size_out) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, buf_cap);
    close(fd);
    if (n < 0) return -1;
    *size_out = (size_t)n;
    return 0;
}

struct Vertex {
    float pos[3];
    float norm[3];
    float col[3];
};

static struct Vertex vertices[GRID_SIZE * GRID_SIZE];
static uint32_t indices[(GRID_SIZE - 1) * (GRID_SIZE - 1) * 6];
static uint32_t num_vertices = GRID_SIZE * GRID_SIZE;
static uint32_t num_indices = (GRID_SIZE - 1) * (GRID_SIZE - 1) * 6;

static float height_fn(float x, float z) {
    return sinf(x * 0.15f) * cosf(z * 0.15f) * 4.5f
         + sinf(x * 0.35f + z * 0.2f) * 2.2f
         + cosf(x * 0.08f) * 3.0f;
}

static void generate_terrain(void) {
    float extent = 40.0f;
    float step = extent / (float)(GRID_SIZE - 1);
    float half = extent * 0.5f;

    for (int z = 0; z < GRID_SIZE; z++) {
        for (int x = 0; x < GRID_SIZE; x++) {
            int idx = z * GRID_SIZE + x;
            float px = (float)x * step - half;
            float pz = (float)z * step - half;
            float py = height_fn(px, pz);

            vertices[idx].pos[0] = px;
            vertices[idx].pos[1] = py;
            vertices[idx].pos[2] = pz;

            // Calculate normal using finite differences
            float dx = height_fn(px + 0.1f, pz) - height_fn(px - 0.1f, pz);
            float dz = height_fn(px, pz + 0.1f) - height_fn(px, pz - 0.1f);
            float nx = -dx / 0.2f;
            float ny = 1.0f;
            float nz = -dz / 0.2f;
            float len = sqrtf(nx*nx + ny*ny + nz*nz);
            vertices[idx].norm[0] = nx / len;
            vertices[idx].norm[1] = ny / len;
            vertices[idx].norm[2] = nz / len;

            // Elevation colors
            if (py < -1.0f) {
                // Low valley (dark green)
                vertices[idx].col[0] = 0.15f;
                vertices[idx].col[1] = 0.45f;
                vertices[idx].col[2] = 0.20f;
            } else if (py < 2.5f) {
                // Mid slope (green)
                vertices[idx].col[0] = 0.25f;
                vertices[idx].col[1] = 0.65f;
                vertices[idx].col[2] = 0.25f;
            } else if (py < 5.0f) {
                // Rocky mountain (brown)
                vertices[idx].col[0] = 0.55f;
                vertices[idx].col[1] = 0.45f;
                vertices[idx].col[2] = 0.35f;
            } else {
                // Snow peak (white)
                vertices[idx].col[0] = 0.95f;
                vertices[idx].col[1] = 0.95f;
                vertices[idx].col[2] = 1.00f;
            }
        }
    }

    uint32_t iidx = 0;
    for (int z = 0; z < GRID_SIZE - 1; z++) {
        for (int x = 0; x < GRID_SIZE - 1; x++) {
            uint32_t v0 = z * GRID_SIZE + x;
            uint32_t v1 = z * GRID_SIZE + (x + 1);
            uint32_t v2 = (z + 1) * GRID_SIZE + x;
            uint32_t v3 = (z + 1) * GRID_SIZE + (x + 1);

            indices[iidx++] = v0;
            indices[iidx++] = v2;
            indices[iidx++] = v1;

            indices[iidx++] = v1;
            indices[iidx++] = v2;
            indices[iidx++] = v3;
        }
    }
}

// 4x4 matrix multiplication: R = A * B
static void mat4_mul(float *R, const float *A, const float *B) {
    float temp[16];
    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < 4; j++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++)
                sum += A[i + k * 4] * B[k + j * 4];
            temp[i + j * 4] = sum;
        }
    }
    memcpy(R, temp, sizeof(temp));
}

// Perspective projection (Vulkan depth [0, 1])
static void mat4_perspective(float *m, float fov_rad, float aspect, float near_z, float far_z) {
    memset(m, 0, 16 * sizeof(float));
    float tan_half = tanf(fov_rad / 2.0f);
    m[0] = 1.0f / (aspect * tan_half);
    m[5] = -1.0f / tan_half; // Vulkan Y is down
    m[10] = far_z / (near_z - far_z);
    m[11] = -1.0f;
    m[14] = -(far_z * near_z) / (far_z - near_z);
}

// LookAt matrix
static void mat4_lookat(float *m, const float *eye, const float *center, const float *up) {
    float fx = center[0] - eye[0];
    float fy = center[1] - eye[1];
    float fz = center[2] - eye[2];
    float flen = sqrtf(fx*fx + fy*fy + fz*fz);
    fx /= flen; fy /= flen; fz /= flen;

    // s = f x up
    float sx = fy * up[2] - fz * up[1];
    float sy = fz * up[0] - fx * up[2];
    float sz = fx * up[1] - fy * up[0];
    float slen = sqrtf(sx*sx + sy*sy + sz*sz);
    sx /= slen; sy /= slen; sz /= slen;

    // u = s x f
    float ux = sy * fz - sz * fy;
    float uy = sz * fx - sx * fz;
    float uz = sx * fy - sy * fx;

    m[0] = sx;  m[4] = sy;  m[8]  = sz;  m[12] = -(sx*eye[0] + sy*eye[1] + sz*eye[2]);
    m[1] = ux;  m[5] = uy;  m[9]  = uz;  m[13] = -(ux*eye[0] + uy*eye[1] + uz*eye[2]);
    m[2] = -fx; m[6] = -fy; m[10] = -fz; m[14] =  (fx*eye[0] + fy*eye[1] + fz*eye[2]);
    m[3] = 0;   m[7] = 0;   m[11] = 0;   m[15] = 1.0f;
}

int main(void) {
    generate_terrain();
    printf("Generated 3D terrain: %u vertices, %u triangles (%u indices)\n",
           num_vertices, num_indices / 3, num_indices);

    xcb_connection_t *conn = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(conn)) {
        printf("xcb_connect failed\n");
        return 1;
    }
    const xcb_setup_t *setup = xcb_get_setup(conn);
    xcb_screen_t *xcb_screen = xcb_setup_roots_iterator(setup).data;
    xcb_window_t win = xcb_generate_id(conn);
    xcb_create_window(conn, XCB_COPY_FROM_PARENT, win, xcb_screen->root,
                      50, 50, WIN_W, WIN_H, 1,
                      XCB_WINDOW_CLASS_INPUT_OUTPUT, xcb_screen->root_visual, 0, NULL);
    xcb_map_window(conn, win);
    xcb_flush(conn);

    const char *driver_path = "/data/data/com.termux/files/home/funnymdzz-mesa/build-bionic/src/panfrost/vulkan/libvulkan_panfrost.so";
    void *lib = dlopen(driver_path, RTLD_NOW);
    if (!lib) {
        printf("dlopen failed: %s\n", dlerror());
        return 1;
    }
    PFN_vkGetInstanceProcAddr icd_gpa =
        (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
    if (!icd_gpa) icd_gpa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");

    #define IPROC(name) (PFN_##name)icd_gpa(instance, #name)

    PFN_vkCreateInstance CreateInstance = (PFN_vkCreateInstance)icd_gpa(NULL, "vkCreateInstance");
    const char *ext_names[] = { "VK_KHR_surface", "VK_KHR_xcb_surface" };
    VkInstanceCreateInfo inst_info = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .enabledExtensionCount = 2,
        .ppEnabledExtensionNames = ext_names
    };
    VkInstance instance;
    CHECK(CreateInstance(&inst_info, NULL, &instance), "vkCreateInstance");

    PFN_vkCreateXcbSurfaceKHR CreateXcbSurfaceKHR = IPROC(vkCreateXcbSurfaceKHR);
    VkXcbSurfaceCreateInfoKHR surf_info = {
        .sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR,
        .connection = conn,
        .window = win
    };
    VkSurfaceKHR surface;
    CHECK(CreateXcbSurfaceKHR(instance, &surf_info, NULL, &surface), "vkCreateXcbSurfaceKHR");

    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = IPROC(vkGetDeviceProcAddr);
    #define DPROC(dev, name) (PFN_##name)GetDeviceProcAddr(dev, #name)

    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices = IPROC(vkEnumeratePhysicalDevices);
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = IPROC(vkGetPhysicalDeviceProperties);
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetSurfaceCapabilitiesKHR = IPROC(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetSurfaceFormatsKHR = IPROC(vkGetPhysicalDeviceSurfaceFormatsKHR);
    PFN_vkGetPhysicalDeviceMemoryProperties GetMemoryProperties = IPROC(vkGetPhysicalDeviceMemoryProperties);
    PFN_vkCreateDevice CreateDevice = IPROC(vkCreateDevice);

    uint32_t pcount = 1;
    VkPhysicalDevice pdev;
    EnumeratePhysicalDevices(instance, &pcount, &pdev);

    VkPhysicalDeviceProperties dev_props;
    GetPhysicalDeviceProperties(pdev, &dev_props);
    printf("=== PanVK 3D Terrain Benchmark ===\n");
    printf("Device:   %s\n", dev_props.deviceName);
    printf("Window:   %dx%d (Termux:X11 display :0)\n", WIN_W, WIN_H);

    VkSurfaceCapabilitiesKHR caps;
    CHECK(GetSurfaceCapabilitiesKHR(pdev, surface, &caps), "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

    uint32_t fmt_count = 0;
    GetSurfaceFormatsKHR(pdev, surface, &fmt_count, NULL);
    VkSurfaceFormatKHR formats[16];
    if (fmt_count > 16) fmt_count = 16;
    GetSurfaceFormatsKHR(pdev, surface, &fmt_count, formats);
    VkSurfaceFormatKHR chosen = formats[0];

    const char *dev_ext_names[] = { "VK_KHR_swapchain" };
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qinfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority
    };
    VkDeviceCreateInfo dinfo = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qinfo,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = dev_ext_names
    };
    VkDevice device;
    CHECK(CreateDevice(pdev, &dinfo, NULL, &device), "vkCreateDevice");

    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = DPROC(device, vkCreateSwapchainKHR);
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = DPROC(device, vkGetSwapchainImagesKHR);
    PFN_vkCreateImageView CreateImageView = DPROC(device, vkCreateImageView);
    PFN_vkCreateRenderPass CreateRenderPass = DPROC(device, vkCreateRenderPass);
    PFN_vkCreateFramebuffer CreateFramebuffer = DPROC(device, vkCreateFramebuffer);
    PFN_vkCreateShaderModule CreateShaderModule = DPROC(device, vkCreateShaderModule);
    PFN_vkCreatePipelineLayout CreatePipelineLayout = DPROC(device, vkCreatePipelineLayout);
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = DPROC(device, vkCreateGraphicsPipelines);
    PFN_vkCreateCommandPool CreateCommandPool = DPROC(device, vkCreateCommandPool);
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = DPROC(device, vkAllocateCommandBuffers);
    PFN_vkBeginCommandBuffer BeginCommandBuffer = DPROC(device, vkBeginCommandBuffer);
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = DPROC(device, vkCmdBeginRenderPass);
    PFN_vkCmdBindPipeline CmdBindPipeline = DPROC(device, vkCmdBindPipeline);
    PFN_vkCmdBindVertexBuffers CmdBindVertexBuffers = DPROC(device, vkCmdBindVertexBuffers);
    PFN_vkCmdBindIndexBuffer CmdBindIndexBuffer = DPROC(device, vkCmdBindIndexBuffer);
    PFN_vkCmdPushConstants CmdPushConstants = DPROC(device, vkCmdPushConstants);
    PFN_vkCmdDrawIndexed CmdDrawIndexed = DPROC(device, vkCmdDrawIndexed);
    PFN_vkCmdEndRenderPass CmdEndRenderPass = DPROC(device, vkCmdEndRenderPass);
    PFN_vkEndCommandBuffer EndCommandBuffer = DPROC(device, vkEndCommandBuffer);
    PFN_vkGetDeviceQueue GetDeviceQueue = DPROC(device, vkGetDeviceQueue);
    PFN_vkQueueSubmit QueueSubmit = DPROC(device, vkQueueSubmit);
    PFN_vkQueueWaitIdle QueueWaitIdle = DPROC(device, vkQueueWaitIdle);
    PFN_vkAcquireNextImageKHR AcquireNextImageKHR = DPROC(device, vkAcquireNextImageKHR);
    PFN_vkQueuePresentKHR QueuePresentKHR = DPROC(device, vkQueuePresentKHR);
    PFN_vkCreateSemaphore CreateSemaphore = DPROC(device, vkCreateSemaphore);
    PFN_vkCreateBuffer CreateBuffer = DPROC(device, vkCreateBuffer);
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = DPROC(device, vkGetBufferMemoryRequirements);
    PFN_vkAllocateMemory AllocateMemory = DPROC(device, vkAllocateMemory);
    PFN_vkBindBufferMemory BindBufferMemory = DPROC(device, vkBindBufferMemory);
    PFN_vkMapMemory MapMemory = DPROC(device, vkMapMemory);
    PFN_vkCreateImage CreateImage = DPROC(device, vkCreateImage);
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = DPROC(device, vkGetImageMemoryRequirements);
    PFN_vkBindImageMemory BindImageMemory = DPROC(device, vkBindImageMemory);

    VkPhysicalDeviceMemoryProperties mem_props;
    GetMemoryProperties(pdev, &mem_props);

    // 1. Create Vertex Buffer
    VkDeviceSize vb_size = sizeof(vertices);
    VkBufferCreateInfo vb_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = vb_size,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer v_buf;
    CHECK(CreateBuffer(device, &vb_info, NULL, &v_buf), "vkCreateBuffer(vertex)");
    VkMemoryRequirements vb_req;
    GetBufferMemoryRequirements(device, v_buf, &vb_req);

    uint32_t host_mem_type = 0;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((vb_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))) {
            host_mem_type = i;
            break;
        }
    }
    VkMemoryAllocateInfo vb_alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = vb_req.size,
        .memoryTypeIndex = host_mem_type
    };
    VkDeviceMemory v_mem;
    CHECK(AllocateMemory(device, &vb_alloc, NULL, &v_mem), "vkAllocateMemory(vertex)");
    CHECK(BindBufferMemory(device, v_buf, v_mem, 0), "vkBindBufferMemory(vertex)");
    void *mapped_vb;
    CHECK(MapMemory(device, v_mem, 0, vb_size, 0, &mapped_vb), "vkMapMemory(vertex)");
    memcpy(mapped_vb, vertices, vb_size);

    // 2. Create Index Buffer
    VkDeviceSize ib_size = sizeof(indices);
    VkBufferCreateInfo ib_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = ib_size,
        .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };
    VkBuffer i_buf;
    CHECK(CreateBuffer(device, &ib_info, NULL, &i_buf), "vkCreateBuffer(index)");
    VkMemoryRequirements ib_req;
    GetBufferMemoryRequirements(device, i_buf, &ib_req);
    VkMemoryAllocateInfo ib_alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = ib_req.size,
        .memoryTypeIndex = host_mem_type
    };
    VkDeviceMemory i_mem;
    CHECK(AllocateMemory(device, &ib_alloc, NULL, &i_mem), "vkAllocateMemory(index)");
    CHECK(BindBufferMemory(device, i_buf, i_mem, 0), "vkBindBufferMemory(index)");
    void *mapped_ib;
    CHECK(MapMemory(device, i_mem, 0, ib_size, 0, &mapped_ib), "vkMapMemory(index)");
    memcpy(mapped_ib, indices, ib_size);

    // 3. Swapchain
    VkSwapchainCreateInfoKHR sc_info = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surface,
        .minImageCount = caps.minImageCount,
        .imageFormat = chosen.format,
        .imageColorSpace = chosen.colorSpace,
        .imageExtent = caps.currentExtent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainKHR swapchain;
    CHECK(CreateSwapchainKHR(device, &sc_info, NULL, &swapchain), "vkCreateSwapchainKHR");

    uint32_t sc_img_count = 0;
    GetSwapchainImagesKHR(device, swapchain, &sc_img_count, NULL);
    VkImage sc_images[8];
    if (sc_img_count > 8) sc_img_count = 8;
    GetSwapchainImagesKHR(device, swapchain, &sc_img_count, sc_images);

    // 4. Depth Buffer (D16_UNORM)
    VkImageCreateInfo depth_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_D16_UNORM,
        .extent = { caps.currentExtent.width, caps.currentExtent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage depth_img;
    CHECK(CreateImage(device, &depth_info, NULL, &depth_img), "vkCreateImage(depth)");
    VkMemoryRequirements depth_req;
    GetImageMemoryRequirements(device, depth_img, &depth_req);
    uint32_t dev_mem_type = 0;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if (depth_req.memoryTypeBits & (1 << i)) { dev_mem_type = i; break; }
    }
    VkMemoryAllocateInfo depth_alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = depth_req.size,
        .memoryTypeIndex = dev_mem_type
    };
    VkDeviceMemory depth_mem;
    CHECK(AllocateMemory(device, &depth_alloc, NULL, &depth_mem), "vkAllocateMemory(depth)");
    CHECK(BindImageMemory(device, depth_img, depth_mem, 0), "vkBindImageMemory(depth)");

    VkImageViewCreateInfo depth_view_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = depth_img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_D16_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 }
    };
    VkImageView depth_view;
    CHECK(CreateImageView(device, &depth_view_info, NULL, &depth_view), "vkCreateImageView(depth)");

    // 5. RenderPass (Color + Depth)
    VkAttachmentDescription attachments[2] = {
        // Color
        {
            .format = chosen.format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        },
        // Depth
        {
            .format = VK_FORMAT_D16_UNORM,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
        }
    };
    VkAttachmentReference color_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference depth_ref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
        .pDepthStencilAttachment = &depth_ref
    };
    VkRenderPassCreateInfo rp_info = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2,
        .pAttachments = attachments,
        .subpassCount = 1,
        .pSubpasses = &subpass
    };
    VkRenderPass render_pass;
    CHECK(CreateRenderPass(device, &rp_info, NULL, &render_pass), "vkCreateRenderPass");

    // 6. Framebuffers
    VkImageView views[8];
    VkFramebuffer framebuffers[8];
    for (uint32_t i = 0; i < sc_img_count; i++) {
        VkImageViewCreateInfo vi = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = sc_images[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = chosen.format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 }
        };
        CHECK(CreateImageView(device, &vi, NULL, &views[i]), "vkCreateImageView(color)");
        VkImageView fb_views[2] = { views[i], depth_view };
        VkFramebufferCreateInfo fb = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = render_pass,
            .attachmentCount = 2,
            .pAttachments = fb_views,
            .width = caps.currentExtent.width,
            .height = caps.currentExtent.height,
            .layers = 1
        };
        CHECK(CreateFramebuffer(device, &fb, NULL, &framebuffers[i]), "vkCreateFramebuffer");
    }

    // 7. Shaders & Pipeline
    size_t vert_size, frag_size;
    uint8_t vert_code[8192], frag_code[8192];
    load_file("/data/data/com.termux/files/home/terrain_vert.spv", vert_code, sizeof(vert_code), &vert_size);
    load_file("/data/data/com.termux/files/home/terrain_frag.spv", frag_code, sizeof(frag_code), &frag_size);

    VkShaderModuleCreateInfo vsmi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = vert_size, .pCode = (uint32_t*)vert_code };
    VkShaderModule vert_module;
    CHECK(CreateShaderModule(device, &vsmi, NULL, &vert_module), "vkCreateShaderModule(vert)");
    VkShaderModuleCreateInfo fsmi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = frag_size, .pCode = (uint32_t*)frag_code };
    VkShaderModule frag_module;
    CHECK(CreateShaderModule(device, &fsmi, NULL, &frag_module), "vkCreateShaderModule(frag)");

    // Push constant range for 4x4 matrix (64 bytes)
    VkPushConstantRange pc_range = {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
        .offset = 0,
        .size = 64
    };
    VkPipelineLayoutCreateInfo pl_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pc_range
    };
    VkPipelineLayout playout;
    CHECK(CreatePipelineLayout(device, &pl_info, NULL, &playout), "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vert_module, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = frag_module, .pName = "main" },
    };

    VkVertexInputBindingDescription bind_desc = {
        .binding = 0,
        .stride = sizeof(struct Vertex),
        .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
    };
    VkVertexInputAttributeDescription attr_descs[3] = {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, pos) },
        { .location = 1, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, norm) },
        { .location = 2, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = offsetof(struct Vertex, col) },
    };
    VkPipelineVertexInputStateCreateInfo vi_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &bind_desc,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attr_descs
    };
    VkPipelineInputAssemblyStateCreateInfo ia_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
    };
    VkViewport viewport = { 0, 0, (float)caps.currentExtent.width, (float)caps.currentExtent.height, 0, 1 };
    VkRect2D scissor = { {0,0}, caps.currentExtent };
    VkPipelineViewportStateCreateInfo vp_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &viewport,
        .scissorCount = 1, .pScissors = &scissor
    };
    VkPipelineRasterizationStateCreateInfo rs_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_BACK_BIT,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f
    };
    VkPipelineMultisampleStateCreateInfo ms_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT
    };
    VkPipelineDepthStencilStateCreateInfo ds_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS,
    };
    VkPipelineColorBlendAttachmentState cb_attach = { .colorWriteMask = 0xF };
    VkPipelineColorBlendStateCreateInfo cb_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cb_attach
    };
    VkGraphicsPipelineCreateInfo gp_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi_state, .pInputAssemblyState = &ia_state,
        .pViewportState = &vp_state, .pRasterizationState = &rs_state,
        .pMultisampleState = &ms_state, .pDepthStencilState = &ds_state,
        .pColorBlendState = &cb_state,
        .layout = playout, .renderPass = render_pass, .subpass = 0,
    };
    VkPipeline pipeline;
    CHECK(CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp_info, NULL, &pipeline), "vkCreateGraphicsPipelines");

    VkCommandPoolCreateInfo cp_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool cpool;
    CHECK(CreateCommandPool(device, &cp_info, NULL, &cpool), "vkCreateCommandPool");

    VkCommandBuffer cmdbufs[8];
    VkCommandBufferAllocateInfo cb_alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = sc_img_count
    };
    CHECK(AllocateCommandBuffers(device, &cb_alloc, cmdbufs), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo sem_info = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquire_sem, present_sem;
    CreateSemaphore(device, &sem_info, NULL, &acquire_sem);
    CreateSemaphore(device, &sem_info, NULL, &present_sem);

    VkQueue queue;
    GetDeviceQueue(device, 0, 0, &queue);

    // Camera setup
    float proj[16];
    mat4_perspective(proj, 60.0f * 3.14159265f / 180.0f,
                     (float)caps.currentExtent.width / (float)caps.currentExtent.height,
                     0.5f, 150.0f);

    printf("Starting 3D terrain flight benchmark (%d frames on screen)...\n", NUM_FRAMES);
    double t_start = get_time_sec();

    for (int frame = 0; frame < NUM_FRAMES; frame++) {
        uint32_t img_idx = 0;
        AcquireNextImageKHR(device, swapchain, 1000000000ULL, acquire_sem, VK_NULL_HANDLE, &img_idx);

        // Orbit camera around terrain
        float angle = (float)frame * 0.015f;
        float eye[3] = { 28.0f * cosf(angle), 16.0f, 28.0f * sinf(angle) };
        float target[3] = { 0.0f, 0.0f, 0.0f };
        float up[3] = { 0.0f, 1.0f, 0.0f };
        float view[16];
        mat4_lookat(view, eye, target, up);

        float mvp[16];
        mat4_mul(mvp, proj, view);

        // Record command buffer for this frame
        VkCommandBuffer cmdbuf = cmdbufs[img_idx];
        VkCommandBufferBeginInfo begin_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        BeginCommandBuffer(cmdbuf, &begin_info);

        VkClearValue clear_vals[2] = {
            { .color = { .float32 = { 0.45f, 0.65f, 0.85f, 1.0f } } }, // Sky blue
            { .depthStencil = { 1.0f, 0 } }
        };
        VkRenderPassBeginInfo rp_begin = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = render_pass,
            .framebuffer = framebuffers[img_idx],
            .renderArea = { {0,0}, caps.currentExtent },
            .clearValueCount = 2,
            .pClearValues = clear_vals
        };
        CmdBeginRenderPass(cmdbuf, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);
        CmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

        VkDeviceSize offset = 0;
        CmdBindVertexBuffers(cmdbuf, 0, 1, &v_buf, &offset);
        CmdBindIndexBuffer(cmdbuf, i_buf, 0, VK_INDEX_TYPE_UINT32);

        CmdPushConstants(cmdbuf, playout, VK_SHADER_STAGE_VERTEX_BIT, 0, 64, mvp);
        CmdDrawIndexed(cmdbuf, num_indices, 1, 0, 0, 0);

        CmdEndRenderPass(cmdbuf);
        EndCommandBuffer(cmdbuf);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit_info = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &acquire_sem, .pWaitDstStageMask = &wait_stage,
            .commandBufferCount = 1, .pCommandBuffers = &cmdbuf,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &present_sem,
        };
        QueueSubmit(queue, 1, &submit_info, VK_NULL_HANDLE);

        VkPresentInfoKHR present_info = {
            .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &present_sem,
            .swapchainCount = 1, .pSwapchains = &swapchain, .pImageIndices = &img_idx,
        };
        QueuePresentKHR(queue, &present_info);
        QueueWaitIdle(queue);
        xcb_flush(conn);
    }

    double t_end = get_time_sec();
    double total_time = t_end - t_start;
    double fps = (double)NUM_FRAMES / total_time;
    double frame_ms = (total_time / (double)NUM_FRAMES) * 1000.0;

    printf("\n=== PanVK 3D Terrain Flight Results ===\n");
    printf("  Resolution:      %dx%d (Termux:X11 display :0)\n", caps.currentExtent.width, caps.currentExtent.height);
    printf("  Triangles/Frame: %u\n", num_indices / 3);
    printf("  Total Frames:    %d\n", NUM_FRAMES);
    printf("  Total Time:      %.3f s\n", total_time);
    printf("  Frame Rate:      %.1f FPS\n", fps);
    printf("  Frame Latency:   %.2f ms / frame\n", frame_ms);

    return 0;
}
