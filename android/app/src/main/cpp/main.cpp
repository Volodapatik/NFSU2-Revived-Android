#include <android_native_app_glue.h>
#include <android/log.h>
#include <jni.h>
#include <vulkan/vulkan.h>
#include <algorithm>
#include <vector>
#include <cmath>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <string>
#include <cstring>
#include <thread>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <cstdlib>
#include <dlfcn.h>
#include <unistd.h>
#include "vertex_formats.h"
#include "guest_memory.h"
#include "game_data.h"
#include "guest_runtime.h"
#include "d3d9_bridge.h"
#include "guest_boot_worker.h"
#include "gamepad_input.h"
#include "vulkan_driver.h"
#include "universal_gpu_loader.h"
#include "nfs_widescreen.h"

extern "C" void nfsD3D9GetDebugStats(uint64_t*,uint64_t*,uint32_t*,uint64_t*,uint32_t*);
extern "C" void nfsD3D9GetBackbufferProbe(uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*);
extern "C" void dxvkAndroidGetPresentStats(uint64_t*,int32_t*,uint64_t*,int32_t*,uint32_t*,uint32_t*);
extern "C" void nfsGuestAudioGetDebugStats(int32_t*,int32_t*,int32_t*,int32_t*,int32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*,uint32_t*);

static android_app* s_active_app = nullptr;
static uint32_t s_vulkanApiVersion = VK_API_VERSION_1_3;
static void showStatus(android_app* app, const char* message);
android_app* getActiveAndroidApp() {
    return s_active_app;
}
void notifyHaltToHost(const char* message) {
    if (s_active_app) {
        showStatus(s_active_app, message);
    }
}

static void showStatus(android_app* app, const char* message) {
    JNIEnv* env = nullptr;
    if (app->activity->vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return;
    jclass cls = env->GetObjectClass(app->activity->clazz);
    jmethodID method = env->GetMethodID(cls, "setStatus", "(Ljava/lang/String;)V");
    if (method) {
        jstring text = env->NewStringUTF(message);
        env->CallVoidMethod(app->activity->clazz, method, text);
        env->DeleteLocalRef(text);
    }
    if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); }
    env->DeleteLocalRef(cls);
    app->activity->vm->DetachCurrentThread();
}

// Independent bring-up target. No guest game code is executed yet.
static void check(VkResult r, const char* operation) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(operation) + ": " + std::to_string(r));
}
#define VK_CHECK(call) check((call), #call)
struct Renderer {
    VkInstance instance{};
    VkSurfaceKHR surface{};
    VkPhysicalDevice gpu{};
    VkDevice device{};
    VkQueue queue{};
    uint32_t family{};
    VkSwapchainKHR swapchain{};
    VkExtent2D extent{};
    VkRenderPass pass{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkSemaphore acquired{};
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> frames;
    std::vector<VkSemaphore> ready;
    uint64_t presented = 0;

    void stop() {
        if (device) {
            vkDeviceWaitIdle(device);
            for (auto f : frames) vkDestroyFramebuffer(device, f, nullptr);
            for (auto v : views) vkDestroyImageView(device, v, nullptr);
            for (auto s : ready) vkDestroySemaphore(device, s, nullptr);
            if (acquired) vkDestroySemaphore(device, acquired, nullptr);
            if (pool) vkDestroyCommandPool(device, pool, nullptr);
            if (pass) vkDestroyRenderPass(device, pass, nullptr);
            if (swapchain) vkDestroySwapchainKHR(device, swapchain, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (surface) vkDestroySurfaceKHR(instance, surface, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        *this = Renderer{};
    }
    void start(ANativeWindow* window) {
        const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "NFSU2 Vulkan bootstrap";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.pApplicationInfo = &app; ci.enabledExtensionCount = 2; ci.ppEnabledExtensionNames = extensions;
        VK_CHECK(vkCreateInstance(&ci, nullptr, &instance));
        VkAndroidSurfaceCreateInfoKHR sci{VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR};
        sci.window = window;
        VK_CHECK(vkCreateAndroidSurfaceKHR(instance, &sci, nullptr, &surface));
        uint32_t count = 0;
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, nullptr));
        std::vector<VkPhysicalDevice> devices(count);
        VK_CHECK(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
        for (auto candidate : devices) {
            VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(candidate, &props);
            if (props.apiVersion < VK_API_VERSION_1_1) continue;
            uint32_t ec = 0;
            VK_CHECK(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &ec, nullptr));
            std::vector<VkExtensionProperties> exts(ec);
            VK_CHECK(vkEnumerateDeviceExtensionProperties(candidate, nullptr, &ec, exts.data()));
            bool hasSwapchain = false;
            for (auto& e : exts) hasSwapchain |= std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0;
            if (!hasSwapchain) continue;
            uint32_t qc = 0; vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qc, nullptr);
            std::vector<VkQueueFamilyProperties> queues(qc);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qc, queues.data());
            for (uint32_t i = 0; i < qc; ++i) {
                VkBool32 present = false;
                VK_CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface, &present));
                if ((queues[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) { gpu = candidate; family = i; break; }
            }
            if (gpu) {
                if (universal_gpu_is_custom()) {
                    s_vulkanApiVersion = universal_gpu_get_api_version();
                } else if (props.apiVersion >= VK_API_VERSION_1_3) {
                    s_vulkanApiVersion = props.apiVersion;
                } else {
                    s_vulkanApiVersion = VK_API_VERSION_1_3;
                }
                __android_log_print(ANDROID_LOG_INFO, "NFSU2", "GPU %s; API %u.%u (Vulkan 1.3 presentation active)", props.deviceName,
                    VK_VERSION_MAJOR(s_vulkanApiVersion), VK_VERSION_MINOR(s_vulkanApiVersion));
                auto vertexMissing = unsupportedD3DVertexFormats(candidate, vkGetPhysicalDeviceFormatProperties);
                __android_log_print(vertexMissing.empty() ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "NFSU2",
                    "D3D9 vertex types without Vulkan vertex fetch: %s", vertexMissing.empty() ? "none" : vertexMissing.c_str());
                break;
            }
        }
        if (!gpu) throw std::runtime_error("No Vulkan 1.1 graphics/present device");
        float priority = 1.f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = family; qci.queueCount = 1; qci.pQueuePriorities = &priority;
        const char* dext = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
        dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = &dext;
        // No optional physical-device features or newer Vulkan extensions.
        VK_CHECK(vkCreateDevice(gpu, &dci, nullptr, &device));
        vkGetDeviceQueue(device, family, 0, &queue);
        VkSurfaceCapabilitiesKHR caps{};
        VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu, surface, &caps));
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) throw std::runtime_error("Surface lacks color attachment usage");
        uint32_t fc = 0; VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fc, nullptr));
        if (!fc) throw std::runtime_error("Surface has no formats");
        std::vector<VkSurfaceFormatKHR> formats(fc);
        VK_CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &fc, formats.data()));
        auto format = formats.front();
        if (format.format == VK_FORMAT_UNDEFINED) format.format = VK_FORMAT_R8G8B8A8_UNORM;
        for (auto f : formats) if (f.format == VK_FORMAT_R8G8B8A8_UNORM || f.format == VK_FORMAT_B8G8R8A8_UNORM) { format = f; break; }
        extent = caps.currentExtent;
        if (extent.width == UINT32_MAX) {
            extent.width = std::clamp(uint32_t(std::max(1, ANativeWindow_getWidth(window))), caps.minImageExtent.width, caps.maxImageExtent.width);
            extent.height = std::clamp(uint32_t(std::max(1, ANativeWindow_getHeight(window))), caps.minImageExtent.height, caps.maxImageExtent.height);
        }
        if (!extent.width || !extent.height) throw std::runtime_error("Surface is minimized");
        VkSwapchainCreateInfoKHR sw{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        sw.surface = surface; sw.minImageCount = caps.minImageCount + 1;
        if (caps.maxImageCount) sw.minImageCount = std::min(sw.minImageCount, caps.maxImageCount);
        sw.imageFormat = format.format; sw.imageColorSpace = format.colorSpace; sw.imageExtent = extent;
        sw.imageArrayLayers = 1; sw.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        sw.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE; sw.preTransform = caps.currentTransform;
        for (auto bit : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
                         VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & bit) { sw.compositeAlpha = bit; break; }
        sw.presentMode = VK_PRESENT_MODE_FIFO_KHR; sw.clipped = VK_TRUE;
        VK_CHECK(vkCreateSwapchainKHR(device, &sw, nullptr, &swapchain));
        uint32_t ic = 0; VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &ic, nullptr));
        std::vector<VkImage> images(ic); VK_CHECK(vkGetSwapchainImagesKHR(device, swapchain, &ic, images.data()));
        VkAttachmentDescription attachment{};
        attachment.format = format.format; attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{}; sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1; sub.pColorAttachments = &ref;
        VkSubpassDependency dep{}; dep.srcSubpass = VK_SUBPASS_EXTERNAL; dep.dstSubpass = 0;
        dep.srcStageMask = dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rp.attachmentCount = 1; rp.pAttachments = &attachment; rp.subpassCount = 1; rp.pSubpasses = &sub;
        rp.dependencyCount = 1; rp.pDependencies = &dep;
        VK_CHECK(vkCreateRenderPass(device, &rp, nullptr, &pass));
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VK_CHECK(vkCreateSemaphore(device, &sem, nullptr, &acquired));
        for (auto image : images) {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView view{}; VK_CHECK(vkCreateImageView(device, &vi, nullptr, &view)); views.push_back(view);
            VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
            fb.renderPass = pass; fb.attachmentCount = 1; fb.pAttachments = &views.back();
            fb.width = extent.width; fb.height = extent.height; fb.layers = 1;
            VkFramebuffer frame{}; VK_CHECK(vkCreateFramebuffer(device, &fb, nullptr, &frame)); frames.push_back(frame);
            VkSemaphore s{}; VK_CHECK(vkCreateSemaphore(device, &sem, nullptr, &s)); ready.push_back(s);
        }
        VkCommandPoolCreateInfo cp{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        cp.queueFamilyIndex = family; cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VK_CHECK(vkCreateCommandPool(device, &cp, nullptr, &pool));
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.commandPool = pool; ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = 1;
        VK_CHECK(vkAllocateCommandBuffers(device, &ca, &command));
        __android_log_print(ANDROID_LOG_INFO, "NFSU2", "Vulkan ready %ux%u; game draw bridge pending", extent.width, extent.height);
    }
    bool draw() {
        uint32_t index;
        auto result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX, acquired, VK_NULL_HANDLE, &index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) return false;
        if (result != VK_SUBOPTIMAL_KHR) check(result, "acquire");
        VK_CHECK(vkResetCommandBuffer(command, 0));
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(command, &begin));
        VkClearValue color{}; color.color = {{0.035f, 0.16f, 0.22f, 1.f}};
        VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rp.renderPass = pass; rp.framebuffer = frames[index]; rp.renderArea.extent = extent;
        rp.clearValueCount = 1; rp.pClearValues = &color;
        vkCmdBeginRenderPass(command, &rp, VK_SUBPASS_CONTENTS_INLINE); vkCmdEndRenderPass(command);
        VK_CHECK(vkEndCommandBuffer(command));
        VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount = 1; submit.pWaitSemaphores = &acquired; submit.pWaitDstStageMask = &waitStage;
        submit.commandBufferCount = 1; submit.pCommandBuffers = &command;
        submit.signalSemaphoreCount = 1; submit.pSignalSemaphores = &ready[index];
        VK_CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        present.waitSemaphoreCount = 1; present.pWaitSemaphores = &ready[index];
        present.swapchainCount = 1; present.pSwapchains = &swapchain; present.pImageIndices = &index;
        result = vkQueuePresentKHR(queue, &present);
        // Deliberately serialized for bring-up; replace with frame fences for the game renderer.
        VK_CHECK(vkQueueWaitIdle(queue));
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) return false;
        check(result, "present");
        if (++presented == 1 || presented % 300 == 0)
            __android_log_print(ANDROID_LOG_INFO, "NFSU2", "Presented frames: %llu", static_cast<unsigned long long>(presented));
        return true;
    }
};
struct Host {
    Renderer renderer;
    bool resumed = false;
    bool failed = false;
    bool guestAttempted = false;
    std::string guestStatus;
    GuestBootWorker guestWorker;
};
static GamepadInput gamepad;
// Device ids the Java side accepted as real gamepads (updateGamepads). Events
// from anything else never reach the pad: several MediaTek phones expose
// sensors and side keys as joysticks whose X axis rests off centre.
static std::mutex gamepadDevicesMutex;
static std::vector<int32_t> gamepadDevices;
static std::atomic<bool> gamepadsRemoved{false};
static bool acceptedGamepad(const AInputEvent* event){
    std::lock_guard<std::mutex> lock(gamepadDevicesMutex);
    return std::find(gamepadDevices.begin(),gamepadDevices.end(),AInputEvent_getDeviceId(event))!=gamepadDevices.end();
}
static void setPadKey(unsigned scan,bool down){setGuestKey(scan,down);}
static void publishPad(){
    auto state=gamepad.padState();
    // Z: LT pushes it one way and RT the other, as DirectInput shows an Xbox pad.
    // Rz (axes[5]) provides right trigger independently for custom remapping.
    const float axes[6]={state.leftX,state.leftY,state.leftTrigger-state.rightTrigger,state.rightX,state.rightY,state.rightTrigger*2.f-1.f};
    setGuestPad(axes,state.buttons,state.hatX,state.hatY);

    static void (*s_stick_cam)(float,float) = nullptr;
    static void (*s_stick_garage)(float,float) = nullptr;
    if(!s_stick_cam || !s_stick_garage){
        void* h=dlopen("libgame_core.so",RTLD_NOLOAD);
        if(!h) h=dlopen("libgame_core.so",RTLD_NOW|RTLD_GLOBAL);
        if(!h) h=dlopen("libgraphics_cache.so",RTLD_NOW|RTLD_GLOBAL);
        if(h) {
            if(!s_stick_cam) s_stick_cam=reinterpret_cast<void(*)(float,float)>(dlsym(h,"nfs_camera_stick_input"));
            if(!s_stick_garage) s_stick_garage=reinterpret_cast<void(*)(float,float)>(dlsym(h,"nfs_garage_stick_input"));
        }
        if(!s_stick_cam) s_stick_cam=reinterpret_cast<void(*)(float,float)>(dlsym(RTLD_DEFAULT,"nfs_camera_stick_input"));
        if(!s_stick_garage) s_stick_garage=reinterpret_cast<void(*)(float,float)>(dlsym(RTLD_DEFAULT,"nfs_garage_stick_input"));
    }
    if(s_stick_cam) s_stick_cam(state.rightX, state.rightY);
    if(s_stick_garage) s_stick_garage(state.rightX, state.rightY);
    static void (*s_roam_stick)(float, float) = nullptr;
    if(!s_roam_stick){
        void* h=dlopen("libnfsu2_roam.so",RTLD_NOW|RTLD_GLOBAL);
        if(h) s_roam_stick=reinterpret_cast<void(*)(float,float)>(dlsym(h,"roam_stick_input"));
        if(!s_roam_stick) s_roam_stick=reinterpret_cast<void(*)(float,float)>(dlsym(RTLD_DEFAULT,"roam_stick_input"));
    }
    if(s_roam_stick) s_roam_stick(state.rightX, state.rightY);
}
static void onCommand(android_app* app, int32_t command) {
    auto& h = *static_cast<Host*>(app->userData);
    if (command == APP_CMD_RESUME) { h.resumed = true; h.failed = false; }
    if (command == APP_CMD_PAUSE) { setGuestPaused(true);gamepad.releaseAll(setPadKey);clearGuestInput();h.resumed = false; h.renderer.stop();acknowledgeD3D9Surface(false);setD3D9HostWindow(nullptr); }
    if (command == APP_CMD_TERM_WINDOW) {
        setGuestPaused(true);h.renderer.stop(); h.failed = false;
        acknowledgeD3D9Surface(false);setD3D9HostWindow(nullptr);
    }
}
// Set when the player actually uses the pad (a button, a stick past half way,
// a trigger), so the touch controls can hide again after a touch revealed
// them; GameActivity polls it with the driving-mode check.
static std::atomic<bool> gamepadUsed{false};
static void notePadUse(const AInputEvent* event){
    if(AInputEvent_getType(event)==AINPUT_EVENT_TYPE_KEY){if(AKeyEvent_getAction(event)==AKEY_EVENT_ACTION_DOWN)gamepadUsed=true;return;}
    for(int32_t axis:{AMOTION_EVENT_AXIS_X,AMOTION_EVENT_AXIS_Y,AMOTION_EVENT_AXIS_Z,AMOTION_EVENT_AXIS_RZ,AMOTION_EVENT_AXIS_RX,AMOTION_EVENT_AXIS_RY,AMOTION_EVENT_AXIS_HAT_X,AMOTION_EVENT_AXIS_HAT_Y})
        if(std::fabs(AMotionEvent_getAxisValue(event,axis,0))>.5f){gamepadUsed=true;return;}
    for(int32_t axis:{AMOTION_EVENT_AXIS_LTRIGGER,AMOTION_EVENT_AXIS_RTRIGGER,AMOTION_EVENT_AXIS_BRAKE,AMOTION_EVENT_AXIS_GAS})
        if(AMotionEvent_getAxisValue(event,axis,0)>.3f){gamepadUsed=true;return;}
}
extern "C" JNIEXPORT jboolean JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeGamepadUsed(JNIEnv*,jclass){return gamepadUsed.exchange(false)?JNI_TRUE:JNI_FALSE;}
static int32_t onInput(android_app*,AInputEvent* event){
    int32_t type=AInputEvent_getType(event);
    gamepad.setKeyEmulation(!guestGamepadInUse());
    if(type==AINPUT_EVENT_TYPE_MOTION){
        if(!acceptedGamepad(event))return 0;
        bool handled=gamepad.motion(event,guestDrivingControls(),setPadKey);if(handled){notePadUse(event);publishPad();}return handled?1:0;
    }
    if(type!=AINPUT_EVENT_TYPE_KEY)return 0;
    // Any key of an accepted pad, whatever source it reports (pads in keyboard
    // mode send KEYBOARD); keys it does not map fall through to the table below.
    if(acceptedGamepad(event)&&gamepad.key(event,guestDrivingControls(),setPadKey)){notePadUse(event);publishPad();return 1;}
    int32_t key=AKeyEvent_getKeyCode(event);unsigned scan=0;
    switch(key){
    case AKEYCODE_DPAD_UP:scan=0xc8;break;case AKEYCODE_DPAD_DOWN:scan=0xd0;break;
    case AKEYCODE_DPAD_LEFT:scan=0xcb;break;case AKEYCODE_DPAD_RIGHT:scan=0xcd;break;
    case AKEYCODE_ENTER:case AKEYCODE_BUTTON_A:scan=0x1c;break;
    case AKEYCODE_ESCAPE:case AKEYCODE_BACK:case AKEYCODE_BUTTON_B:scan=1;break;
    case AKEYCODE_SPACE:case AKEYCODE_BUTTON_X:scan=0x39;break;
    case AKEYCODE_DEL:scan=0xe;break;
    case AKEYCODE_SHIFT_LEFT:case AKEYCODE_BUTTON_Y:scan=0x2a;break;
    case AKEYCODE_CTRL_LEFT:scan=0x1d;break;case AKEYCODE_TAB:scan=0xf;break;
    case AKEYCODE_BUTTON_THUMBL:case AKEYCODE_BUTTON_11:case AKEYCODE_BUTTON_13:case AKEYCODE_BUTTON_15:scan=0x31;break;
    case AKEYCODE_BUTTON_THUMBR:case AKEYCODE_BUTTON_12:case AKEYCODE_BUTTON_14:case AKEYCODE_BUTTON_16:scan=0x2e;break;
    default:{static const unsigned letters[]={0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
        if(key>=AKEYCODE_A&&key<=AKEYCODE_Z)scan=letters[key-AKEYCODE_A];
        else if(key>=AKEYCODE_0&&key<=AKEYCODE_9)scan=key==AKEYCODE_0?0xb:unsigned(key-AKEYCODE_0)+1;
        break;}
    }
    if(!scan)return 0;int action=AKeyEvent_getAction(event);
    if(action!=AKEY_EVENT_ACTION_DOWN&&action!=AKEY_EVENT_ACTION_UP)return 0;
    setGuestKey(scan,action==AKEY_EVENT_ACTION_DOWN);return 1;
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeKey(JNIEnv*,jclass,jint scan,jboolean down){
    setGuestKey(unsigned(scan),down==JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeKeyNav(JNIEnv*,jclass,jint scan,jboolean down){
    // Optional roam-only selector hook. The actual guest key state is already
    // sent by nativeKey above, so this remains safe when the roam library is absent.
    (void)scan;
    (void)down;
}
extern "C" JNIEXPORT jboolean JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeDrivingControls(JNIEnv*,jclass){return guestDrivingControls()?JNI_TRUE:JNI_FALSE;}
extern "C" JNIEXPORT jstring JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeGetRenderDiagnostics(JNIEnv* env,jclass){
    uint64_t createAttempts=0,createSuccesses=0,d3dPresents=0,vkAcquires=0,vkPresents=0;
    uint32_t lastCreate=0xffffffffu,lastD3dPresent=0xffffffffu,width=0,height=0;
    int32_t lastAcquire=0,lastVkPresent=0;
    nfsD3D9GetDebugStats(&createAttempts,&createSuccesses,&lastCreate,&d3dPresents,&lastD3dPresent);
    dxvkAndroidGetPresentStats(&vkAcquires,&lastAcquire,&vkPresents,&lastVkPresent,&width,&height);
    uint32_t probeState=0,probePoints=0,probeNonBlack=0,probeWidth=0,probeHeight=0,probeFormat=0,probeMs=0,probeResult=0xffffffffu;
    nfsD3D9GetBackbufferProbe(&probeState,&probePoints,&probeNonBlack,&probeWidth,&probeHeight,&probeFormat,&probeMs,&probeResult);
    D3D9RenderSnapshot renderSnapshot{};
    nfsD3D9GetRenderSnapshot(&renderSnapshot);
    int32_t audioActive=0,audioXruns=-1,audioBuffer=-1,audioCapacity=-1,audioBurst=-1;
    uint32_t audioRate=0,audioCallbackPeak=0,audioCallbackAge=0,audioMaxCallbackAge=0,audioProducerGap=0,audioRing=0,audioStalls=0;
    nfsGuestAudioGetDebugStats(&audioActive,&audioXruns,&audioBuffer,&audioCapacity,&audioBurst,&audioRate,&audioCallbackPeak,
        &audioCallbackAge,&audioMaxCallbackAge,&audioProducerGap,&audioRing,&audioStalls);
    char createStatus[16],d3dStatus[16],acquireStatus[16],vkPresentStatus[16];
    if(createAttempts)std::snprintf(createStatus,sizeof(createStatus),"0x%08X",lastCreate);else std::snprintf(createStatus,sizeof(createStatus),"pending");
    if(d3dPresents)std::snprintf(d3dStatus,sizeof(d3dStatus),"0x%08X",lastD3dPresent);else std::snprintf(d3dStatus,sizeof(d3dStatus),"pending");
    if(vkAcquires)std::snprintf(acquireStatus,sizeof(acquireStatus),"0x%08X",uint32_t(lastAcquire));else std::snprintf(acquireStatus,sizeof(acquireStatus),"pending");
    if(vkPresents)std::snprintf(vkPresentStatus,sizeof(vkPresentStatus),"0x%08X",uint32_t(lastVkPresent));else std::snprintf(vkPresentStatus,sizeof(vkPresentStatus),"pending");
    char probeText[160],audioText[192],audioGapText[192];
    if(probeState==0)std::snprintf(probeText,sizeof(probeText),"Backbuffer probe: armed; waits for 120 D3D9 frames");
    else if(probeState==4)std::snprintf(probeText,sizeof(probeText),"Backbuffer probe: sampling once...");
    else if(probeState==1)std::snprintf(probeText,sizeof(probeText),"Backbuffer: non-black=%u/%u, %ux%u fmt=%u (%u ms)",probeNonBlack,probePoints,probeWidth,probeHeight,probeFormat,probeMs);
    else if(probeState==3)std::snprintf(probeText,sizeof(probeText),"Backbuffer probe unsupported: fmt=%u result=0x%08X",probeFormat,probeResult);
    else std::snprintf(probeText,sizeof(probeText),"Backbuffer probe failed: 0x%08X, %ux%u fmt=%u (%u ms)",probeResult,probeWidth,probeHeight,probeFormat,probeMs);
    if(!audioActive)std::snprintf(audioText,sizeof(audioText),"Audio: AAudio stream inactive/unavailable");
    else if(audioRate&&audioBuffer>=0)std::snprintf(audioText,sizeof(audioText),"Audio: xruns=%d buffer=%d/%d frames (%.1f ms), burst=%d, %u Hz",
        audioXruns,audioBuffer,audioCapacity,1000.0*audioBuffer/audioRate,audioBurst,audioRate);
    else std::snprintf(audioText,sizeof(audioText),"Audio: xruns=%d buffer=%d/%d frames, burst=%d, %u Hz",audioXruns,audioBuffer,audioCapacity,audioBurst,audioRate);
    std::snprintf(audioGapText,sizeof(audioGapText),"Audio peaks: callback=%u us, callback age=%u/%u ms, update gap=%u ms, ring=%u ms, stalls=%u",
        audioCallbackPeak,audioCallbackAge==UINT32_MAX?UINT32_MAX:audioCallbackAge/1000,
        audioMaxCallbackAge==UINT32_MAX?UINT32_MAX:audioMaxCallbackAge/1000,audioProducerGap/1000,audioRing/1000,audioStalls);
    if(audioCallbackAge==UINT32_MAX)std::snprintf(audioGapText,sizeof(audioGapText),"Audio peaks: callback=%u us, callback age=n/a, max age=n/a, update gap=%u ms, ring=%u ms, stalls=%u",
        audioCallbackPeak,audioProducerGap/1000,audioRing/1000,audioStalls);
    else if(audioMaxCallbackAge==UINT32_MAX)std::snprintf(audioGapText,sizeof(audioGapText),"Audio peaks: callback=%u us, callback age=%u ms, max age=n/a, update gap=%u ms, ring=%u ms, stalls=%u",
        audioCallbackPeak,audioCallbackAge/1000,audioProducerGap/1000,audioRing/1000,audioStalls);
    const auto& beginScene=renderSnapshot.methods[static_cast<uint32_t>(D3D9RenderStat::BeginScene)];
    const auto& clear=renderSnapshot.methods[static_cast<uint32_t>(D3D9RenderStat::Clear)];
    const auto& endScene=renderSnapshot.methods[static_cast<uint32_t>(D3D9RenderStat::EndScene)];
    const auto& setRenderTarget=renderSnapshot.methods[static_cast<uint32_t>(D3D9RenderStat::SetRenderTarget)];
    const auto& stretchRect=renderSnapshot.methods[static_cast<uint32_t>(D3D9RenderStat::StretchRect)];
    const auto& drawIndexed=renderSnapshot.methods[static_cast<uint32_t>(D3D9RenderStat::DrawIndexedPrimitive)];
    const auto& drawUP=renderSnapshot.methods[static_cast<uint32_t>(D3D9RenderStat::DrawPrimitiveUP)];
    const char* drawPath=drawIndexed.lastPath==1?"forwarded":drawIndexed.lastPath==2?"culled":"none";
    const char* drawUPPath=drawUP.lastPath==1?"forwarded":drawUP.lastPath==2?"culled":drawUP.calls?"pending":"none";
    const auto& target=renderSnapshot.activeTarget;
    const char* targetKind=target.state==0?"pending":target.state==1?"reading":target.state==3?"unavailable":
        target.isImplicitBackbuffer==1?"backbuffer":target.isImplicitBackbuffer==2?"offscreen":"unknown";
    const auto& targetBefore=renderSnapshot.helperTargetBefore;
    const auto& targetAfter=renderSnapshot.helperTargetAfter;
    const auto& queue0=renderSnapshot.bucketHelpers[0];
    const auto& queue1=renderSnapshot.bucketHelpers[1];
    const auto& helperUP=renderSnapshot.helperUP;
    const char* helperUPState=helperUP.state==0?"not seen":helperUP.state==1?"metadata only":helperUP.state==2?"validated": "args rejected";
    const char* helperUPData=helperUP.dataStatus==0?"not checked":helperUP.dataStatus==1?"copied":helperUP.dataStatus==2?"unreadable":
        helperUP.dataStatus==3?"unexpected shape":helperUP.dataStatus==4?"oversize":"unknown";
    const auto targetName=[](const D3D9RenderTargetSnapshot& value)->const char*{
        return value.state==0?"pending":value.state==1?"reading":value.state==3?"unavailable":
            value.isImplicitBackbuffer==1?"backbuffer":value.isImplicitBackbuffer==2?"offscreen":"unknown";
    };
    const auto& guestRender=renderSnapshot.guestRender;
    const auto& flow=renderSnapshot.guestFlow;
    char renderText[3584];
    std::snprintf(renderText,sizeof(renderText),"BScene=%u/%u hr=%08X pc=%08X; Clear=%u/%u hr=%08X pc=%08X\n"
        "EndScene=%u/%u hr=%08X pc=%08X; SetRT=%u/%u hr=%08X pc=%08X\n"
        "Stretch=%u/%u hr=%08X pc=%08X; DrawIdx=%u/%u/%u last=%s hr=%08X pc=%08X\n"
        "DrawUP=%u/%u/%u last=%s hr=%08X pc=%08X\n"
        "Active RT0=%s %ux%u fmt=%u use=%08X pool=%u ms=%u/%u type=%u hr=%08X pc=%08X\n"
        "Helper RT0 pre=%s %ux%u fmt=%u use=%08X pool=%u ms=%u/%u type=%u hr=%08X pc=%08X\n"
        "Helper RT0 post=%s %ux%u fmt=%u use=%08X pool=%u ms=%u/%u type=%u hr=%08X pc=%08X\n"
        "Guest calls=%llu buckets=%llu empty=%llu nonempty=%llu last=%08X/%08X; route idx=%llu up=%llu last=%08X obj=%08X\n"
        "Idx count +/<=0=%llu/%llu last=%u max=%u; UP count +/<=0=%llu/%llu last=%u max=%u; 0x8383AC=%08X reads=%llu\n"
        "Q0(CE9E0) calls=%llu existing-skip=%llu alloc-null(P/A)=%llu/%llu bucket-selects=%llu links(nonzero/zero)=%llu/%llu invalid=%llu last=%08X bank/index=%u/%u\n"
        " Q0 bank0 sel/link/touched/hot=%llu/%llu/%llu/%u:%u; bank1=%llu/%llu/%llu/%u:%u\n"
        "Q1(CEAE0) calls=%llu existing-skip=%llu alloc-null(P/A)=%llu/%llu bucket-selects=%llu links(nonzero/zero)=%llu/%llu invalid=%llu last=%08X bank/index=%u/%u\n"
        " Q1 bank0 sel/link/touched/hot=%llu/%llu/%llu/%u:%u; bank1=%llu/%llu/%llu/%u:%u\n"
        "HelperUP %s data=%s caller=%08X type/count=%u/%u verts=%u stride=%u bytes=%u ptr=%08X FVF hr=%08X val=%08X tex=%u type=%u hr=%08X\n"
        " TSS mask=%02X coord=%08X xform=%08X color=%u/%u/%u alpha=%u/%u/%u; pos X=%g..%g Y=%g..%g Z=%g..%g\n"
        " RGBA min=%u,%u,%u,%u max=%u,%u,%u,%u; UV min=%g,%g,%g,%g max=%g,%g,%g,%g",
        beginScene.calls,beginScene.forwarded,beginScene.lastResult,beginScene.lastCaller,
        clear.calls,clear.forwarded,clear.lastResult,clear.lastCaller,
        endScene.calls,endScene.forwarded,endScene.lastResult,endScene.lastCaller,
        setRenderTarget.calls,setRenderTarget.forwarded,setRenderTarget.lastResult,setRenderTarget.lastCaller,
        stretchRect.calls,stretchRect.forwarded,stretchRect.lastResult,stretchRect.lastCaller,
        drawIndexed.calls,drawIndexed.forwarded,drawIndexed.culled,drawPath,drawIndexed.lastResult,drawIndexed.lastCaller,
        drawUP.calls,drawUP.forwarded,drawUP.culled,drawUPPath,drawUP.lastResult,drawUP.lastCaller,
        targetKind,target.width,target.height,target.format,target.usage,target.pool,target.multisample,target.multisampleQuality,
        target.type,target.lastResult,target.lastCaller,
        targetName(targetBefore),targetBefore.width,targetBefore.height,targetBefore.format,targetBefore.usage,targetBefore.pool,
        targetBefore.multisample,targetBefore.multisampleQuality,targetBefore.type,targetBefore.lastResult,targetBefore.lastCaller,
        targetName(targetAfter),targetAfter.width,targetAfter.height,targetAfter.format,targetAfter.usage,targetAfter.pool,
        targetAfter.multisample,targetAfter.multisampleQuality,targetAfter.type,targetAfter.lastResult,targetAfter.lastCaller,
        static_cast<unsigned long long>(guestRender.calls),static_cast<unsigned long long>(guestRender.bucketChecks),
        static_cast<unsigned long long>(guestRender.bucketsEmpty),static_cast<unsigned long long>(guestRender.bucketsNonempty),
        guestRender.lastBucketAddress,guestRender.lastBucketHead,static_cast<unsigned long long>(guestRender.routeIndexed),
        static_cast<unsigned long long>(guestRender.routeUp),guestRender.lastRouteValue,guestRender.lastObjectAddress,
        static_cast<unsigned long long>(guestRender.indexedCountPositive),static_cast<unsigned long long>(guestRender.indexedCountNonPositive),
        guestRender.lastIndexedCount,guestRender.maxIndexedCount,static_cast<unsigned long long>(guestRender.upCountPositive),
        static_cast<unsigned long long>(guestRender.upCountNonPositive),guestRender.lastUpCount,guestRender.maxUpCount,
        renderSnapshot.guestGlobal8383ACValue,static_cast<unsigned long long>(renderSnapshot.guestGlobal8383ACReads),
        static_cast<unsigned long long>(queue0.calls),static_cast<unsigned long long>(queue0.existingNodeSkips),
        static_cast<unsigned long long>(queue0.primaryAllocatorNull),static_cast<unsigned long long>(queue0.auxiliaryAllocatorNull),
        static_cast<unsigned long long>(queue0.bucketSelections),static_cast<unsigned long long>(queue0.linkedNonNull),
        static_cast<unsigned long long>(queue0.linkedNull),static_cast<unsigned long long>(queue0.invalidBucketAddresses),
        queue0.lastBucketAddress,queue0.lastBank,queue0.lastBucketIndex,
        static_cast<unsigned long long>(queue0.bankSelections[0]),static_cast<unsigned long long>(queue0.bankLinkedNonNull[0]),
        static_cast<unsigned long long>(queue0.touchedBuckets[0]),queue0.hottestBucket[0],queue0.hottestBucketHits[0],
        static_cast<unsigned long long>(queue0.bankSelections[1]),static_cast<unsigned long long>(queue0.bankLinkedNonNull[1]),
        static_cast<unsigned long long>(queue0.touchedBuckets[1]),queue0.hottestBucket[1],queue0.hottestBucketHits[1],
        static_cast<unsigned long long>(queue1.calls),static_cast<unsigned long long>(queue1.existingNodeSkips),
        static_cast<unsigned long long>(queue1.primaryAllocatorNull),static_cast<unsigned long long>(queue1.auxiliaryAllocatorNull),
        static_cast<unsigned long long>(queue1.bucketSelections),static_cast<unsigned long long>(queue1.linkedNonNull),
        static_cast<unsigned long long>(queue1.linkedNull),static_cast<unsigned long long>(queue1.invalidBucketAddresses),
        queue1.lastBucketAddress,queue1.lastBank,queue1.lastBucketIndex,
        static_cast<unsigned long long>(queue1.bankSelections[0]),static_cast<unsigned long long>(queue1.bankLinkedNonNull[0]),
        static_cast<unsigned long long>(queue1.touchedBuckets[0]),queue1.hottestBucket[0],queue1.hottestBucketHits[0],
        static_cast<unsigned long long>(queue1.bankSelections[1]),static_cast<unsigned long long>(queue1.bankLinkedNonNull[1]),
        static_cast<unsigned long long>(queue1.touchedBuckets[1]),queue1.hottestBucket[1],queue1.hottestBucketHits[1],
        helperUPState,helperUPData,helperUP.caller,helperUP.primitiveType,helperUP.primitiveCount,helperUP.vertexCount,
        helperUP.stride,helperUP.byteCount,helperUP.guestVertexAddress,helperUP.fvfResult,helperUP.fvf,
        helperUP.textureBound,helperUP.textureType,helperUP.textureResult,helperUP.stageStateValidMask,
        helperUP.texCoordIndex,helperUP.textureTransformFlags,helperUP.colorOp,helperUP.colorArg1,helperUP.colorArg2,
        helperUP.alphaOp,helperUP.alphaArg1,helperUP.alphaArg2,
        helperUP.positionMin[0],helperUP.positionMax[0],helperUP.positionMin[1],helperUP.positionMax[1],
        helperUP.positionMin[2],helperUP.positionMax[2],
        helperUP.colorMin[0],helperUP.colorMin[1],helperUP.colorMin[2],helperUP.colorMin[3],
        helperUP.colorMax[0],helperUP.colorMax[1],helperUP.colorMax[2],helperUP.colorMax[3],
        helperUP.texcoordMin[0],helperUP.texcoordMax[0],helperUP.texcoordMin[1],helperUP.texcoordMax[1],
        helperUP.texcoordMin[2],helperUP.texcoordMax[2],helperUP.texcoordMin[3],helperUP.texcoordMax[3]);
    char flowText[1600];
    std::snprintf(flowText,sizeof(flowText),
        "Flow 5D2AD6 hit/take/skip=%llu/%llu/%llu cmp=%08X/%08X; 5D2B1D hit/call/skip=%llu/%llu/%llu 864F4C/count=%08X/%08X\n"
        "Flow entries CAA/30DE0/316D0/162E0/20AE0=%llu/%llu/%llu/%llu/%llu; CAA 870CD4=%08X zero=%llu\n"
        "30DE0 mode=%08X eq6=%llu global=%08X zero=%llu count=%08X zero=%llu obj=%08X\n"
        "316D0 global=%08X zero=%llu list890080=%08X empty=%llu obj=%08X\n"
        "Q0 callsite 6173D1/621D96/621E79/6220B5/622EB2=%llu/%llu/%llu/%llu/%llu\n"
        "Q1 5C5880 entry=%llu inputCount=%08X zero/positive=%llu/%llu qualifying=%llu record=%08X aux=%08X",
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_GATE_5D2AD6].hits),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_GATE_5D2AD6].taken),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_GATE_5D2AD6].notTaken),
        flow[NFS_GUEST_FLOW_GATE_5D2AD6].lastValue,flow[NFS_GUEST_FLOW_GATE_5D2AD6].lastAuxiliary,
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_GATE_5D2B1D].hits),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_GATE_5D2B1D].taken),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_GATE_5D2B1D].notTaken),
        flow[NFS_GUEST_FLOW_GATE_5D2B1D].lastValue,flow[NFS_GUEST_FLOW_GATE_5D2B1D].lastAuxiliary,
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_CAA20_ENTRY].hits),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_630DE0_ENTRY].hits),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_316D0_ENTRY].hits),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_162E0_ENTRY].hits),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_20AE0_ENTRY].hits),
        flow[NFS_GUEST_FLOW_CAA20_870CD4].lastValue,
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_CAA20_870CD4].taken),
        flow[NFS_GUEST_FLOW_630DE0_8654A4].lastValue,
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_630DE0_8654A4].taken),
        flow[NFS_GUEST_FLOW_630DE0_8026C8].lastValue,
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_630DE0_8026C8].taken),
        flow[NFS_GUEST_FLOW_630DE0_INPUT_COUNT].lastValue,
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_630DE0_INPUT_COUNT].taken),
        flow[NFS_GUEST_FLOW_630DE0_INPUT_OBJECT].lastValue,
        flow[NFS_GUEST_FLOW_316D0_8026C8].lastValue,
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_316D0_8026C8].taken),
        flow[NFS_GUEST_FLOW_316D0_890080].lastValue,
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_316D0_890080].taken),
        flow[NFS_GUEST_FLOW_316D0_INPUT_OBJECT].lastValue,
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_Q0_SITE_6173D1].hits),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_Q0_SITE_621D96].hits),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_Q0_SITE_621E79].hits),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_Q0_SITE_6220B5].hits),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_Q0_SITE_622EB2].hits),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_C5880_ENTRY].hits),
        flow[NFS_GUEST_FLOW_C5880_INPUT_COUNT].lastValue,
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_C5880_INPUT_COUNT].taken),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_C5880_INPUT_COUNT].notTaken),
        static_cast<unsigned long long>(flow[NFS_GUEST_FLOW_C5880_QUALIFYING_RECORD].hits),
        flow[NFS_GUEST_FLOW_C5880_QUALIFYING_RECORD].lastValue,
        flow[NFS_GUEST_FLOW_C5880_QUALIFYING_RECORD].lastAuxiliary);
    char text[6144];
    std::snprintf(text,sizeof(text),"D3D9 Create: %llu calls, %llu ok, %s\nD3D9 Present: %llu calls, %s\nVK acquire: %llu, %s\nVK queuePresent: %llu, %s\nSurface: %ux%u\n%s\n%s\n%s\n%s\n%s",
        static_cast<unsigned long long>(createAttempts),static_cast<unsigned long long>(createSuccesses),createStatus,
        static_cast<unsigned long long>(d3dPresents),d3dStatus,
        static_cast<unsigned long long>(vkAcquires),acquireStatus,
        static_cast<unsigned long long>(vkPresents),vkPresentStatus,width,height,probeText,renderText,flowText,audioText,audioGapText);
    return env->NewStringUTF(text);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativePointer(JNIEnv*,jclass,jfloat x,jfloat y,jint phase){
    setGuestPointer(x,y,phase);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativePointerMiddle(JNIEnv*,jclass,jfloat x,jfloat y,jint phase,jfloat dx,jfloat dy){
    setGuestPointerMiddle(x,y,phase,dx,dy);
    static void (*s_garage_drag)(float, float, int) = nullptr;
    if (!s_garage_drag) {
        void* h = dlopen("libgame_core.so", RTLD_NOLOAD);
        if (!h) h = dlopen("libgame_core.so", RTLD_NOW | RTLD_GLOBAL);
        if (h) s_garage_drag = reinterpret_cast<void(*)(float,float,int)>(dlsym(h, "nfs_garage_two_finger_drag"));
        if (!s_garage_drag) s_garage_drag = reinterpret_cast<void(*)(float,float,int)>(dlsym(RTLD_DEFAULT, "nfs_garage_two_finger_drag"));
    }
    if (s_garage_drag) {
        s_garage_drag(dx, dy, phase != 2 ? 1 : 0);
    }
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativePointerMiddle(JNIEnv* env,jclass clazz,jfloat x,jfloat y,jint phase,jfloat dx,jfloat dy){
    Java_io_mininxd_nfsu2_GameActivity_nativePointerMiddle(env,clazz,x,y,phase,dx,dy);
}
static void touchCameraStick(float x,float y){
    static void (*s_stick_cam)(float,float)=nullptr;
    if(!s_stick_cam){
        void* h=dlopen("libgame_core.so",RTLD_NOLOAD);
        if(!h)h=dlopen("libgame_core.so",RTLD_NOW|RTLD_GLOBAL);
        if(!h)h=dlopen("libgraphics_cache.so",RTLD_NOW|RTLD_GLOBAL);
        if(h)s_stick_cam=reinterpret_cast<void(*)(float,float)>(dlsym(h,"nfs_camera_stick_input"));
        if(!s_stick_cam)s_stick_cam=reinterpret_cast<void(*)(float,float)>(dlsym(RTLD_DEFAULT,"nfs_camera_stick_input"));
    }
    if(s_stick_cam)s_stick_cam(x,y);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeTouchCamera(JNIEnv*,jclass,jfloat x,jfloat y){
    touchCameraStick(x,y);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeTypeText(JNIEnv* env,jclass,jstring value,jboolean replace){
    if(!value)return;
    const jchar* chars=env->GetStringChars(value,nullptr);if(!chars)return;
    jsize length=env->GetStringLength(value);
    std::vector<uint32_t> characters;if(replace==JNI_TRUE)characters.assign(40,8);
    for(jsize i=0;i<length&&i<40;++i)if(chars[i]>=0x20&&chars[i]<0x7f)characters.push_back(chars[i]);
    env->ReleaseStringChars(value,chars);
    typeGuestText(characters);
    __android_log_print(ANDROID_LOG_INFO,"NFSU2","Android text delivered to guest: %d characters",int(length));
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeLanguage(JNIEnv* env,jclass,jstring language){
    const char* value=env->GetStringUTFChars(language,nullptr);if(!value)return;
    setGuestLanguage(value);env->ReleaseStringUTFChars(language,value);
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeLanguage(JNIEnv* env,jclass,jstring language){
    const char* value=env->GetStringUTFChars(language,nullptr);if(!value)return;
    setGuestLanguage(value);env->ReleaseStringUTFChars(language,value);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeResolution(JNIEnv*,jclass,jint width,jint height){setGuestResolution(unsigned(width),unsigned(height));}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeFrameLimit(JNIEnv*,jclass,jint cap){setGuestFrameLimit(unsigned(cap));}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeFrameLimit(JNIEnv*,jclass,jint cap){setGuestFrameLimit(unsigned(cap));}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeGamepadLayout(JNIEnv*,jclass,jboolean playstationNames){
    setGuestPadButtonNames(playstationNames==JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeGamepadLayout(JNIEnv*,jclass,jboolean playstationNames){
    setGuestPadButtonNames(playstationNames==JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeGamepadConnected(JNIEnv*,jclass,jboolean connected){setGuestPadConnected(connected==JNI_TRUE);}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeGamepadConnected(JNIEnv*,jclass,jboolean connected){setGuestPadConnected(connected==JNI_TRUE);}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeGamepadDevices(JNIEnv* env,jclass,jintArray ids){
    std::vector<int32_t> devices;
    if(ids){jsize count=env->GetArrayLength(ids);devices.resize(size_t(count));if(count)env->GetIntArrayRegion(ids,0,count,devices.data());}
    std::lock_guard<std::mutex> lock(gamepadDevicesMutex);
    if(devices!=gamepadDevices)__android_log_print(ANDROID_LOG_INFO,"NFS","Accepted gamepads: %zu device(s)",devices.size());
    if(devices.size()<gamepadDevices.size())gamepadsRemoved=true;
    gamepadDevices=std::move(devices);
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeGamepadDevices(JNIEnv* env,jclass cls,jintArray ids){
    Java_io_mininxd_nfsu2_GameActivity_nativeGamepadDevices(env,cls,ids);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeWidescreen(JNIEnv*,jclass,jboolean enabled){setGuestWidescreen(enabled==JNI_TRUE);}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeWidescreen(JNIEnv*,jclass,jboolean enabled){setGuestWidescreen(enabled==JNI_TRUE);}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeScreenMode(JNIEnv*,jclass,jint mode){setGuestScreenMode(int(mode));}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeScreenMode(JNIEnv*,jclass,jint mode){setGuestScreenMode(int(mode));}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeDrawDistance(JNIEnv*,jclass,jfloat distance){
    nfs_widescreen_set_draw_distance(float(distance));
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeDrawDistance(JNIEnv*,jclass,jfloat distance){
    nfs_widescreen_set_draw_distance(float(distance));
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeForwardCulling(JNIEnv*,jclass,jboolean enabled){
    nfs_widescreen_set_forward_culling(enabled == JNI_TRUE ? 1 : 0);
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeForwardCulling(JNIEnv*,jclass,jboolean enabled){
    nfs_widescreen_set_forward_culling(enabled == JNI_TRUE ? 1 : 0);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeHudSafeArea(JNIEnv*,jclass,jboolean enabled){setGuestHudSafeArea(enabled==JNI_TRUE);}
// Minimap at the top-left: the bottom-left corner sits under the steering thumb.
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeOnlineServer(JNIEnv* env,jclass,jstring host){
    const char* text=env->GetStringUTFChars(host,nullptr);setGuestOnlineServer(text?text:"");env->ReleaseStringUTFChars(host,text);
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeOnlineServer(JNIEnv* env,jclass,jstring host){
    const char* text=env->GetStringUTFChars(host,nullptr);setGuestOnlineServer(text?text:"");env->ReleaseStringUTFChars(host,text);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeMinimapTop(JNIEnv*,jclass,jboolean top){nfs_widescreen_set_minimap_offset(top==JNI_TRUE?-230.0f:0.0f);}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeMinimapTop(JNIEnv*,jclass,jboolean top){nfs_widescreen_set_minimap_offset(top==JNI_TRUE?-230.0f:0.0f);}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeHudLogLayout(JNIEnv*,jclass,jboolean enabled){
    nfs_widescreen_set_bank_left(enabled==JNI_TRUE?1:0);
    nfs_widescreen_set_sms_nav_offset(enabled==JNI_TRUE?50.0f:0.0f);
}
static bool compatCpuTextures=false;static unsigned compatBackBuffers=2;
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeBackBuffers(JNIEnv*,jclass,jint count){
    compatBackBuffers=unsigned(count);setD3D9Compatibility(compatCpuTextures,compatBackBuffers);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeCpuTextures(JNIEnv*,jclass,jboolean enabled){
    compatCpuTextures=enabled==JNI_TRUE;setD3D9Compatibility(compatCpuTextures,compatBackBuffers);
}
// A driver imported in the launcher replaces the system one for DXVK. The bootstrap
// renderer, which links the system loader, is then skipped so one process never
// drives the GPU through two Vulkan drivers.
static bool customVulkanDriver=false;
static std::string customVulkanError;
static std::string s_vulkanDriverDesc;
extern "C" void dxvkAndroidSetInstanceProcAddr(PFN_vkGetInstanceProcAddr entry);
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeGpuDriver(JNIEnv* env,jclass,jstring hooks,jstring temp,jstring directory,jstring library){
    auto text=[&](jstring value){const char* c=value?env->GetStringUTFChars(value,nullptr):nullptr;std::string r=c?c:"";if(c)env->ReleaseStringUTFChars(value,c);return r;};
    std::string h=text(hooks),t=text(temp),d=text(directory),l=text(library);
    if(l.empty())return;
    customVulkanDriver=true;
    if (!universal_gpu_init(h.c_str(), t.c_str(), d.c_str(), l.c_str())) {
        customVulkanError = "Could not load Vulkan driver " + l + ": " + universal_gpu_get_error() + ". Choose system driver in the launcher.";
        return;
    }
    auto entry = universal_gpu_get_proc_addr();
    if(!entry){customVulkanError="Could not load Vulkan driver "+l+". Choose system driver in the launcher.";return;}
    dxvkAndroidSetInstanceProcAddr(entry);
    s_vulkanDriverDesc = l;
    s_vulkanApiVersion = universal_gpu_get_api_version();
    if (s_vulkanApiVersion < VK_API_VERSION_1_3) {
        s_vulkanApiVersion = VK_API_VERSION_1_3;
    }
    __android_log_print(ANDROID_LOG_INFO, "NFSU2", "Custom Vulkan driver active: %s (API %u.%u, Device: %s)",
        l.c_str(), VK_API_VERSION_MAJOR(s_vulkanApiVersion), VK_API_VERSION_MINOR(s_vulkanApiVersion),
        universal_gpu_get_device_name());
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeGpuDriver(JNIEnv* env,jclass cls,jstring hooks,jstring temp,jstring directory,jstring library){
    Java_io_mininxd_nfsu2_GameActivity_nativeGpuDriver(env,cls,hooks,temp,directory,library);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeSetAudioMuted(JNIEnv*,jclass,jboolean muted){
    setGuestAudioMuted(muted == JNI_TRUE);
}
extern "C" JNIEXPORT void JNICALL Java_io_mininxd_nfsu2_GameActivity_nativeRequestAudioRestart(JNIEnv*,jclass){
    requestGuestAudioRestart();
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeKey(JNIEnv* env,jclass cls,jint scan,jboolean down){
    Java_io_mininxd_nfsu2_GameActivity_nativeKey(env,cls,scan,down);
}
extern "C" JNIEXPORT jboolean JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeDrivingControls(JNIEnv* env,jclass cls){
    return Java_io_mininxd_nfsu2_GameActivity_nativeDrivingControls(env,cls);
}
extern "C" JNIEXPORT jboolean JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeGamepadUsed(JNIEnv* env,jclass cls){
    return Java_io_mininxd_nfsu2_GameActivity_nativeGamepadUsed(env,cls);
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeTypeText(JNIEnv* env,jclass cls,jstring value,jboolean replace){
    Java_io_mininxd_nfsu2_GameActivity_nativeTypeText(env,cls,value,replace);
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativePointer(JNIEnv* env,jclass cls,jfloat x,jfloat y,jint phase){
    Java_io_mininxd_nfsu2_GameActivity_nativePointer(env,cls,x,y,phase);
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeResolution(JNIEnv* env,jclass cls,jint width,jint height){
    Java_io_mininxd_nfsu2_GameActivity_nativeResolution(env,cls,width,height);
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeBackBuffers(JNIEnv* env,jclass cls,jint count){
    Java_io_mininxd_nfsu2_GameActivity_nativeBackBuffers(env,cls,count);
}
extern "C" JNIEXPORT void JNICALL Java_com_nfsu2_androidevolved_GameActivity_nativeCpuTextures(JNIEnv* env,jclass cls,jboolean enabled){
    Java_io_mininxd_nfsu2_GameActivity_nativeCpuTextures(env,cls,enabled);
}
static void configurePipelineCache(android_app* app){
    const char* files=app->activity->internalDataPath;
    setenv("DXVK_STATE_CACHE","0",1); // Keep startup usable if cache storage is unavailable.
    if(!files||!*files)return;
    // Renderer cache epoch 2: avoid stale caches from the old DXVK preparation.
    // Keep the directory name in sync with GraphicsCache.java.
    auto directory=std::filesystem::path(files).parent_path()/"cache"/"dxvk-v2";
    std::error_code error;std::filesystem::create_directories(directory,error);
    if(error||access(directory.c_str(),W_OK)!=0){
        __android_log_print(ANDROID_LOG_WARN,"NFSU2","DXVK pipeline state cache unavailable; continuing without it");return;
    }
    setenv("DXVK_STATE_CACHE_PATH",directory.c_str(),1);
    setenv("DXVK_STATE_CACHE","1",1);
    __android_log_print(ANDROID_LOG_INFO,"NFSU2","DXVK persistent pipeline state cache: %s",directory.c_str());
}
void android_main(android_app* app) {
    s_active_app = app;
    configurePipelineCache(app);
    Host host; app->userData = &host; app->onAppCmd = onCommand;app->onInputEvent=onInput;
    try {
        testGuestMemory();
        __android_log_print(ANDROID_LOG_INFO, "NFSU2", "Guest memory self-check passed (32-bit offsets on ARM64)");
    } catch (const std::exception& e) {
        __android_log_print(ANDROID_LOG_ERROR, "NFSU2", "%s", e.what());
        ANativeActivity_finish(app->activity);
        host.failed = true;
    }
    while (!app->destroyRequested) {
        int events; android_poll_source* source = nullptr;
        const bool active = host.resumed && app->window && !host.failed;
        int id = ALooper_pollOnce(active ? (d3d9RequestsSurface()?16:0) : -1, nullptr, &events, reinterpret_cast<void**>(&source));
        if (id >= 0 && source) source->process(app, source);
        if(gamepadsRemoved.exchange(false)){gamepad.releaseAll(setPadKey);publishPad();}
        gamepad.setKeyEmulation(!guestGamepadInUse());gamepad.refresh(guestDrivingControls(),setPadKey);
        if (app->destroyRequested) break;
        if (!host.resumed || !app->window || host.failed) continue;
        try {
            setD3D9HostWindow(app->window);
            if(d3d9RequestsSurface()){host.renderer.stop();acknowledgeD3D9Surface(true);}
            setGuestPaused(false);
            if(!customVulkanError.empty())throw std::runtime_error(customVulkanError);
            if (!host.renderer.device&&!d3d9RequestsSurface()&&!(customVulkanDriver&&host.guestAttempted)) {
                checkGameData();
                if(customVulkanDriver){
                    setGuestDisplaySize(unsigned(ANativeWindow_getWidth(app->window)),unsigned(ANativeWindow_getHeight(app->window)));
                }else{
                    host.renderer.start(app->window);
                    setGuestDisplaySize(host.renderer.extent.width,host.renderer.extent.height);
                }
                if (!host.guestAttempted) {
                    host.guestAttempted = true;
                    host.guestWorker.start(std::string(kGameRoot) + "/SPEED2.EXE");
                }
            }
            auto status=host.guestWorker.status();
            if(status!=host.guestStatus){
                host.guestStatus=status;
                std::string apiBanner = "Guest boot in progress; first rendered frame not confirmed\n";
                showStatus(app,("Accessible data: " + getGameRoot() + "\n" + apiBanner + status).c_str());
            }
            if (host.renderer.device&&!host.renderer.draw()) host.renderer.stop();
        } catch (const std::exception& e) {
            __android_log_print(ANDROID_LOG_ERROR, "NFSU2", "%s", e.what());
            showStatus(app, e.what());
            host.renderer.stop(); host.failed = true;
        }
    }
    host.renderer.stop();
    acknowledgeD3D9Surface(false);setD3D9HostWindow(nullptr);
}
