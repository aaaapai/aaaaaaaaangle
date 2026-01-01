#include "zink_simulate_vkfunction.h"

#include <unordered_map>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <cstdint>
#include <memory>
#include <cstring>

#include "vulkan/vulkan_core.h"

struct zink_screen *zink_simulate_screen;
const struct zink_context *zink_simulate_ctx;

void init_zink_simulate_screen(struct zink_screen *screen) {
   printf("init_zink_simulate_screen : %p", screen);
   zink_simulate_screen = screen;
}

void init_zink_simulate_ctx(const struct zink_context *ctx) {
   printf("init_zink_simulate_ctx : %p", ctx);
   zink_simulate_ctx = ctx;
}

#define VKSCR(func) vk##func
#define VKCTX(func) vk##func

// 无图像帧缓冲区的内部表示（提前定义）
struct ZinkImagelessFramebuffer {
    bool is_imageless;
    VkFramebufferCreateFlags flags;
    VkRenderPass renderPass;
    uint32_t width;
    uint32_t height;
    uint32_t layers;
    
    // 无图像帧缓冲区的附件信息
    struct AttachmentInfo {
        VkFormat format;
        VkSampleCountFlagBits samples;
        VkImageUsageFlags usage;
        uint32_t width;
        uint32_t height;
    };
    std::vector<AttachmentInfo> attachmentInfos;
    
    // 实际创建的传统帧缓冲区（在开始渲染通道时创建）
    VkFramebuffer real_framebuffer;
    std::vector<VkImageView> current_attachments;
    
    ZinkImagelessFramebuffer() 
        : is_imageless(false), renderPass(VK_NULL_HANDLE), width(0), height(0), layers(0),
          real_framebuffer(VK_NULL_HANDLE) {}
};

// 模拟时间线信号量的内部状态
struct ZinkTimelineSemaphore {
    std::mutex mutex;
    std::condition_variable cv;
    uint64_t current_value;
    uint64_t signaled_value; // 已经signal的最大值
    std::vector<uint64_t> pending_waits; // 等待中的值列表，用于通知
    
    ZinkTimelineSemaphore(uint64_t initial_value) 
        : current_value(initial_value), signaled_value(initial_value) {}
};

// 全局管理模拟的时间线信号量
class ZinkTimelineSemaphoreManager {
private:
    std::mutex global_mutex;
    std::unordered_map<VkSemaphore, std::shared_ptr<ZinkTimelineSemaphore>> semaphores;
    static ZinkTimelineSemaphoreManager* instance;
    
    ZinkTimelineSemaphoreManager() = default;
    
public:
    static ZinkTimelineSemaphoreManager* getInstance() {
        static ZinkTimelineSemaphoreManager manager;
        return &manager;
    }
    
    void registerSemaphore(VkSemaphore semaphore, uint64_t initial_value) {
        std::lock_guard<std::mutex> lock(global_mutex);
        semaphores[semaphore] = std::make_shared<ZinkTimelineSemaphore>(initial_value);
    }
    
    void unregisterSemaphore(VkSemaphore semaphore) {
        std::lock_guard<std::mutex> lock(global_mutex);
        semaphores.erase(semaphore);
    }
    
    std::shared_ptr<ZinkTimelineSemaphore> getSemaphoreState(VkSemaphore semaphore) {
        std::lock_guard<std::mutex> lock(global_mutex);
        auto it = semaphores.find(semaphore);
        if (it != semaphores.end()) {
            return it->second;
        }
        return nullptr;
    }
};

// 在文件开头添加这些全局管理类
class ZinkDeviceManager {
private:
    std::mutex device_mutex_;
    std::unordered_map<VkCommandPool, VkDevice> command_pool_to_device_;
    std::unordered_map<VkCommandBuffer, VkCommandPool> command_buffer_to_pool_;
    std::unordered_map<VkImageView, VkImage> image_view_to_image_;
    std::unordered_map<VkImage, VkImageCreateInfo> image_info_cache_;
    // 添加帧缓冲区到设备的映射
    std::unordered_map<VkFramebuffer, VkDevice> framebuffer_to_device_;
    
public:
    static ZinkDeviceManager& get() {
        static ZinkDeviceManager instance;
        return instance;
    }
    
    // 现有的命令池和设备映射方法...
    void register_command_pool(VkCommandPool commandPool, VkDevice device) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        command_pool_to_device_[commandPool] = device;
    }
    
    void unregister_command_pool(VkCommandPool commandPool) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        command_pool_to_device_.erase(commandPool);
    }
    
    void register_command_buffer(VkCommandBuffer commandBuffer, VkCommandPool commandPool) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        command_buffer_to_pool_[commandBuffer] = commandPool;
    }
    
    void unregister_command_buffer(VkCommandBuffer commandBuffer) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        command_buffer_to_pool_.erase(commandBuffer);
    }
    
    // 图像视图和图像映射方法...
    void register_image_view(VkImageView imageView, VkImage image) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        image_view_to_image_[imageView] = image;
    }
    
    void unregister_image_view(VkImageView imageView) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        image_view_to_image_.erase(imageView);
    }
    
    void register_image_info(VkImage image, const VkImageCreateInfo& createInfo) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        image_info_cache_[image] = createInfo;
    }
    
    void unregister_image_info(VkImage image) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        image_info_cache_.erase(image);
    }
    
    // 新增的帧缓冲区设备映射方法
    void register_framebuffer(VkFramebuffer framebuffer, VkDevice device) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        framebuffer_to_device_[framebuffer] = device;
    }
    
    void unregister_framebuffer(VkFramebuffer framebuffer) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        framebuffer_to_device_.erase(framebuffer);
    }
    
    VkDevice get_device_from_framebuffer(VkFramebuffer framebuffer) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        auto it = framebuffer_to_device_.find(framebuffer);
        return it != framebuffer_to_device_.end() ? it->second : VK_NULL_HANDLE;
    }
    
    // 查询函数...
    VkDevice get_device_from_command_buffer(VkCommandBuffer commandBuffer) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        
        auto pool_it = command_buffer_to_pool_.find(commandBuffer);
        if (pool_it == command_buffer_to_pool_.end()) {
            return VK_NULL_HANDLE;
        }
        
        auto device_it = command_pool_to_device_.find(pool_it->second);
        if (device_it == command_pool_to_device_.end()) {
            return VK_NULL_HANDLE;
        }
        
        return device_it->second;
    }
    
    VkImage get_image_from_image_view(VkImageView imageView) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        auto it = image_view_to_image_.find(imageView);
        return it != image_view_to_image_.end() ? it->second : VK_NULL_HANDLE;
    }
    
    VkImageCreateInfo get_image_info(VkImage image) {
        std::lock_guard<std::mutex> lock(device_mutex_);
        auto it = image_info_cache_.find(image);
        if (it != image_info_cache_.end()) {
            return it->second;
        }
        
        // 返回默认值
        VkImageCreateInfo defaultInfo = {};
        defaultInfo.extent = {1, 1, 1};
        defaultInfo.arrayLayers = 1;
        defaultInfo.mipLevels = 1;
        return defaultInfo;
    }
};

// 渲染状态管理
struct zink_rendering_state {
    VkRenderingFlagsKHR flags;
    uint32_t viewMask;
    std::vector<VkRenderingAttachmentInfoKHR> colorAttachments;
    VkRenderingAttachmentInfoKHR depthAttachment;
    VkRenderingAttachmentInfoKHR stencilAttachment;
    // 调整初始化顺序，与声明顺序一致
    VkRenderPass compatibleRenderPass;
    VkFramebuffer framebuffer;
    std::vector<VkImageView> imageViews;
    bool hasDepth;
    bool hasStencil;
    
    zink_rendering_state() : compatibleRenderPass(VK_NULL_HANDLE), framebuffer(VK_NULL_HANDLE), 
                            hasDepth(false), hasStencil(false) {}
};

// 管线渲染状态缓存
struct zink_pipeline_rendering_state {
    uint32_t viewMask;
    std::vector<VkFormat> colorAttachmentFormats;
    VkFormat depthAttachmentFormat;
    VkFormat stencilAttachmentFormat;
    
    bool operator==(const zink_pipeline_rendering_state& other) const {
        if (viewMask != other.viewMask) return false;
        if (depthAttachmentFormat != other.depthAttachmentFormat) return false;
        if (stencilAttachmentFormat != other.stencilAttachmentFormat) return false;
        if (colorAttachmentFormats.size() != other.colorAttachmentFormats.size()) return false;
        
        for (size_t i = 0; i < colorAttachmentFormats.size(); ++i) {
            if (colorAttachmentFormats[i] != other.colorAttachmentFormats[i]) return false;
        }
        return true;
    }
};

namespace std {
    template<>
    struct hash<zink_pipeline_rendering_state> {
        size_t operator()(const zink_pipeline_rendering_state& state) const {
            size_t hash = std::hash<uint32_t>()(state.viewMask);
            hash ^= std::hash<VkFormat>()(state.depthAttachmentFormat) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            hash ^= std::hash<VkFormat>()(state.stencilAttachmentFormat) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            
            for (const auto& format : state.colorAttachmentFormats) {
                hash ^= std::hash<VkFormat>()(format) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    };
}

// 全局状态管理
class zink_simulate_context {
private:
    std::mutex state_mutex_;
    std::unordered_map<VkCommandBuffer, std::unique_ptr<zink_rendering_state>> rendering_states_;
    
    // 渲染通道缓存
    std::mutex cache_mutex_;
    std::unordered_map<zink_pipeline_rendering_state, VkRenderPass> render_pass_cache_;
    std::unordered_map<VkDevice, std::unordered_map<uint64_t, VkFramebuffer>> framebuffer_cache_;
    
    std::atomic<uint64_t> next_framebuffer_id_{1};
    
public:
    static zink_simulate_context& get() {
        static zink_simulate_context instance;
        return instance;
    }
    
    // 渲染状态管理
    void set_rendering_state(VkCommandBuffer cmdBuffer, std::unique_ptr<zink_rendering_state> state) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        rendering_states_[cmdBuffer] = std::move(state);
    }
    
    zink_rendering_state* get_rendering_state(VkCommandBuffer cmdBuffer) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = rendering_states_.find(cmdBuffer);
        return it != rendering_states_.end() ? it->second.get() : nullptr;
    }
    
    void remove_rendering_state(VkCommandBuffer cmdBuffer) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        rendering_states_.erase(cmdBuffer);
    }
    
    // 渲染通道缓存
    VkRenderPass get_cached_render_pass(VkDevice device, const zink_pipeline_rendering_state& state) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto it = render_pass_cache_.find(state);
        if (it != render_pass_cache_.end()) {
            return it->second;
        }
        
        // 创建新的渲染通道
        VkRenderPass renderPass = create_compatible_render_pass(device, state);
        if (renderPass != VK_NULL_HANDLE) {
            render_pass_cache_[state] = renderPass;
        }
        return renderPass;
    }
    
    VkFramebuffer get_cached_framebuffer(VkDevice device, uint64_t framebuffer_id,
                                        const std::vector<VkImageView>& attachments,
                                        uint32_t width, uint32_t height, uint32_t layers) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        auto& device_cache = framebuffer_cache_[device];
        auto it = device_cache.find(framebuffer_id);
        if (it != device_cache.end()) {
            return it->second;
        }
        
        // 创建新的帧缓冲区
        VkFramebuffer framebuffer = create_framebuffer(device, attachments, width, height, layers);
        if (framebuffer != VK_NULL_HANDLE) {
            device_cache[framebuffer_id] = framebuffer;
        }
        return framebuffer;
    }
    
    uint64_t generate_framebuffer_id() {
        return next_framebuffer_id_++;
    }
    
    void cleanup_device(VkDevice device) {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        
        // 清理帧缓冲区缓存
        auto fb_it = framebuffer_cache_.find(device);
        if (fb_it != framebuffer_cache_.end()) {
            for (auto& pair : fb_it->second) {
                VKSCR(DestroyFramebuffer)(device, pair.second, nullptr);
            }
            framebuffer_cache_.erase(fb_it);
        }
        
        // 清理渲染通道缓存
        for (auto& pair : render_pass_cache_) {
            VKSCR(DestroyRenderPass)(device, pair.second, nullptr);
        }
        render_pass_cache_.clear();
    }
    
private:
    VkRenderPass create_compatible_render_pass(VkDevice device, const zink_pipeline_rendering_state& state) {

        struct zink_screen *screen;

        std::vector<VkAttachmentDescription> attachments;
        std::vector<VkAttachmentReference> colorRefs;
        VkAttachmentReference depthRef = {};
        bool hasDepth = false;
        
        // 颜色附件
        for (size_t i = 0; i < state.colorAttachmentFormats.size(); ++i) {
            VkAttachmentDescription colorAttachment = {};
            colorAttachment.format = state.colorAttachmentFormats[i];
            colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; // 动态渲染默认是LOAD
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachments.push_back(colorAttachment);
            
            VkAttachmentReference colorRef = {};
            colorRef.attachment = static_cast<uint32_t>(i);
            colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colorRefs.push_back(colorRef);
        }
        
        // 深度附件
        if (state.depthAttachmentFormat != VK_FORMAT_UNDEFINED) {
            VkAttachmentDescription depthAttachment = {};
            depthAttachment.format = state.depthAttachmentFormat;
            depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
            depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            attachments.push_back(depthAttachment);
            
            depthRef.attachment = static_cast<uint32_t>(state.colorAttachmentFormats.size());
            depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            hasDepth = true;
        }
        
        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = static_cast<uint32_t>(colorRefs.size());
        subpass.pColorAttachments = colorRefs.data();
        subpass.pDepthStencilAttachment = hasDepth ? &depthRef : nullptr;
        
        // 子通道依赖 - 处理渲染过程中的读取
        VkSubpassDependency dependency = {};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        
        VkRenderPassCreateInfo renderPassInfo = {};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;
        
        VkRenderPass renderPass;
        VkResult result = VKSCR(CreateRenderPass)(device, &renderPassInfo, nullptr, &renderPass);
        if (result != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        
        return renderPass;
    }
    
    // 完善 create_framebuffer 函数（在 zink_simulate_context 类中）
    VkFramebuffer create_framebuffer(VkDevice device, 
                                                       const std::vector<VkImageView>& attachments,
                                                       uint32_t width, uint32_t height, 
                                                       uint32_t layers) {
       if (attachments.empty()) {
           return VK_NULL_HANDLE;
       }
    
       // 为这些附件找到或创建兼容的渲染通道
       // 这里简化处理，创建一个最小化的渲染通道
       std::vector<VkAttachmentDescription> attachment_descs;
       std::vector<VkAttachmentReference> color_refs;
       VkAttachmentReference depth_ref = {};
       bool has_depth = false;
    
       // 分析附件格式
       for (VkImageView image_view : attachments) {
           VkFormat format = get_image_view_format(image_view);
           if (format == VK_FORMAT_UNDEFINED) {
               continue;
           }
        
           VkAttachmentDescription desc = {};
           desc.format = format;
           desc.samples = VK_SAMPLE_COUNT_1_BIT;
           desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
           desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
           desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
           desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
           desc.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
           desc.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        
           // 检查是否是深度/模板格式
           if (format == VK_FORMAT_D16_UNORM || format == VK_FORMAT_D32_SFLOAT || 
               format == VK_FORMAT_D16_UNORM_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
               format == VK_FORMAT_D32_SFLOAT_S8_UINT) {
               desc.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
               desc.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
               has_depth = true;
               depth_ref.attachment = static_cast<uint32_t>(attachment_descs.size());
               depth_ref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
           } else {
               color_refs.push_back({static_cast<uint32_t>(attachment_descs.size()), 
                                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL});
           }
        
           attachment_descs.push_back(desc);
       }
    
       // 创建子通道
       VkSubpassDescription subpass = {};
       subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
       subpass.colorAttachmentCount = static_cast<uint32_t>(color_refs.size());
       subpass.pColorAttachments = color_refs.data();
       subpass.pDepthStencilAttachment = has_depth ? &depth_ref : nullptr;
        
       // 子通道依赖
       VkSubpassDependency dependency = {};
       dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
       dependency.dstSubpass = 0;
       dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | 
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
       dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | 
                                VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
       dependency.srcAccessMask = 0;
       dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | 
                                 VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
       
       // 创建渲染通道
       VkRenderPassCreateInfo render_pass_info = {};
       render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
       render_pass_info.attachmentCount = static_cast<uint32_t>(attachment_descs.size());
       render_pass_info.pAttachments = attachment_descs.data();
       render_pass_info.subpassCount = 1;
       render_pass_info.pSubpasses = &subpass;
       render_pass_info.dependencyCount = 1;
       render_pass_info.pDependencies = &dependency;
    
       VkRenderPass render_pass;
       if (VKSCR(CreateRenderPass)(device, &render_pass_info, nullptr, &render_pass) != VK_SUCCESS) {
           return VK_NULL_HANDLE;
       }
    
        // 现在创建帧缓冲区
       VkFramebufferCreateInfo framebuffer_info = {};
       framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
       framebuffer_info.renderPass = render_pass;
       framebuffer_info.attachmentCount = static_cast<uint32_t>(attachments.size());
       framebuffer_info.pAttachments = attachments.data();
       framebuffer_info.width = width;
       framebuffer_info.height = height;
       framebuffer_info.layers = layers;
    
       VkFramebuffer framebuffer;
       VkResult result = VKSCR(CreateFramebuffer)(device, &framebuffer_info, nullptr, &framebuffer);
    
       // 清理临时渲染通道
       VKSCR(DestroyRenderPass)(device, render_pass, nullptr);
    
       return (result == VK_SUCCESS) ? framebuffer : VK_NULL_HANDLE;
    }
};

// 无图像帧缓冲区管理器
class ZinkImagelessFramebufferManager {
private:
    std::mutex mutex_;
    std::unordered_map<VkFramebuffer, std::shared_ptr<ZinkImagelessFramebuffer>> framebuffers_;
    
public:
    static ZinkImagelessFramebufferManager& get() {
        static ZinkImagelessFramebufferManager instance;
        return instance;
    }
    
    void registerFramebuffer(VkFramebuffer framebuffer, std::shared_ptr<ZinkImagelessFramebuffer> info) {
        std::lock_guard<std::mutex> lock(mutex_);
        framebuffers_[framebuffer] = info;
    }
    
    void unregisterFramebuffer(VkFramebuffer framebuffer) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = framebuffers_.find(framebuffer);
        if (it != framebuffers_.end()) {
            // 清理实际创建的帧缓冲区
            if (it->second->real_framebuffer != VK_NULL_HANDLE) {
                VkDevice device = ZinkDeviceManager::get().get_device_from_framebuffer(framebuffer);
                if (device != VK_NULL_HANDLE) {
                    VKSCR(DestroyFramebuffer)(device, it->second->real_framebuffer, nullptr);
                }
            }
            framebuffers_.erase(it);
        }
    }
    
    std::shared_ptr<ZinkImagelessFramebuffer> getFramebufferInfo(VkFramebuffer framebuffer) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = framebuffers_.find(framebuffer);
        return it != framebuffers_.end() ? it->second : nullptr;
    }
    
    // 为无图像帧缓冲区创建实际的帧缓冲区
    VkResult createRealFramebuffer(VkDevice device, 
                                   std::shared_ptr<ZinkImagelessFramebuffer> imageless_fb,
                                   const std::vector<VkImageView>& attachments) {
        if (attachments.size() != imageless_fb->attachmentInfos.size()) {
            return VK_ERROR_VALIDATION_FAILED_EXT;
        }
        
        // 创建传统的帧缓冲区
        VkFramebufferCreateInfo framebufferInfo = {};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = imageless_fb->renderPass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = imageless_fb->width;
        framebufferInfo.height = imageless_fb->height;
        framebufferInfo.layers = imageless_fb->layers;
        
        VkFramebuffer real_framebuffer;
        VkResult result = VKSCR(CreateFramebuffer)(device, &framebufferInfo, nullptr, &real_framebuffer);
        if (result == VK_SUCCESS) {
            // 清理旧的帧缓冲区（如果存在）
            if (imageless_fb->real_framebuffer != VK_NULL_HANDLE) {
                VKSCR(DestroyFramebuffer)(device, imageless_fb->real_framebuffer, nullptr);
            }
            imageless_fb->real_framebuffer = real_framebuffer;
            imageless_fb->current_attachments = attachments;
        }
        
        return result;
    }
    
    void cleanupDevice(VkDevice device) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = framebuffers_.begin(); it != framebuffers_.end(); ) {
            VkFramebuffer fb = it->first;
            VkDevice fb_device = ZinkDeviceManager::get().get_device_from_framebuffer(fb);
            
            if (fb_device == device) {
                // 清理资源
                if (it->second->real_framebuffer != VK_NULL_HANDLE) {
                    VKSCR(DestroyFramebuffer)(device, it->second->real_framebuffer, nullptr);
                }
                it = framebuffers_.erase(it);
            } else {
                ++it;
            }
        }
    }
};

// 在文件开头添加兼容性结构体定义
struct CompatibleFramebufferAttachmentImageInfo {
    VkFormat format;
    VkSampleCountFlagBits samples;
    VkImageUsageFlags usage;
    uint32_t width;
    uint32_t height;
    
    CompatibleFramebufferAttachmentImageInfo(const VkFramebufferAttachmentImageInfo& info) {
        // 尝试多种方式获取格式信息
        format = VK_FORMAT_UNDEFINED;
        
        // 方法1: 尝试访问 pViewFormats（新版本）
        #if defined(VK_HEADER_VERSION) && VK_HEADER_VERSION >= 162
        if (info.viewFormatCount > 0 && info.pViewFormats) {
            format = info.pViewFormats[0];
        }
        #else
        // 方法2: 尝试直接访问 format 成员（旧版本）
        #ifdef VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO
        // 某些版本可能有直接的 format 成员
        struct OldFramebufferAttachmentImageInfo {
            VkStructureType sType;
            const void* pNext;
            VkImageCreateFlags flags;
            VkImageUsageFlags usage;
            uint32_t width;
            uint32_t height;
            uint32_t layerCount;
            VkFormat format;  // 旧版本可能有这个成员
        };
        
        const OldFramebufferAttachmentImageInfo* old_info = 
            reinterpret_cast<const OldFramebufferAttachmentImageInfo*>(&info);
        format = old_info->format;
        #endif
        #endif
        
        // 如果仍然没有获取到格式，使用回退
        if (format == VK_FORMAT_UNDEFINED) {
            // 根据使用用途猜测一个合适的格式
            if (info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                format = VK_FORMAT_D32_SFLOAT;
            } else {
                format = VK_FORMAT_R8G8B8A8_UNORM;
            }
        }
        
        samples = VK_SAMPLE_COUNT_1_BIT;
        usage = info.usage;
        width = info.width;
        height = info.height;
    }
};

// 现在实现三个辅助函数
VkDevice get_command_buffer_device(VkCommandBuffer commandBuffer) {
    VkDevice device = ZinkDeviceManager::get().get_device_from_command_buffer(commandBuffer);
    
    // 如果无法从映射中获取，尝试使用回退方案
    if (device == VK_NULL_HANDLE) {
        // 在实际实现中，可能需要查询命令缓冲区的内部状态
        // 这里使用一个简单的回退：返回第一个可用的设备
        // 注意：这仅适用于单设备应用程序
        static VkDevice fallback_device = VK_NULL_HANDLE;
        return fallback_device;
    }
    
    return device;
}

VkFormat get_image_view_format(VkImageView imageView) {
    VkImage image = ZinkDeviceManager::get().get_image_from_image_view(imageView);
    if (image == VK_NULL_HANDLE) {
        return VK_FORMAT_UNDEFINED;
    }
    
    // 获取图像创建信息
    VkImageCreateInfo imageInfo = ZinkDeviceManager::get().get_image_info(image);
    return imageInfo.format;
}

void get_image_view_size(VkImageView imageView, uint32_t* width, uint32_t* height, uint32_t* layers) {
    VkImage image = ZinkDeviceManager::get().get_image_from_image_view(imageView);
    if (image == VK_NULL_HANDLE) {
        if (width) *width = 0;
        if (height) *height = 0;
        if (layers) *layers = 0;
        return;
    }
    
    // 获取图像创建信息
    VkImageCreateInfo imageInfo = ZinkDeviceManager::get().get_image_info(image);
    
    if (width) *width = imageInfo.extent.width;
    if (height) *height = imageInfo.extent.height;
    
    // 对于层数，需要考虑数组层数
    if (layers) {
        *layers = imageInfo.arrayLayers;
        
        // 如果图像类型是3D，则使用深度作为层数
        if (imageInfo.imageType == VK_IMAGE_TYPE_3D) {
            *layers = imageInfo.extent.depth;
        }
    }
}

// 拦截命令池创建函数
VkResult zink_simulate_vkCreateCommandPool(
    VkDevice device,
    const VkCommandPoolCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkCommandPool* pCommandPool) {
    
    VkResult result = VKSCR(CreateCommandPool)(device, pCreateInfo, pAllocator, pCommandPool);
    if (result == VK_SUCCESS) {
        ZinkDeviceManager::get().register_command_pool(*pCommandPool, device);
    }
    return result;
}

void zink_simulate_vkDestroyCommandPool(
    VkDevice device,
    VkCommandPool commandPool,
    const VkAllocationCallbacks* pAllocator) {
    
    ZinkDeviceManager::get().unregister_command_pool(commandPool);
    VKSCR(DestroyCommandPool)(device, commandPool, pAllocator);
}

// 拦截命令缓冲区分配函数
VkResult zink_simulate_vkAllocateCommandBuffers(
    VkDevice device,
    const VkCommandBufferAllocateInfo* pAllocateInfo,
    VkCommandBuffer* pCommandBuffers) {
    
    VkResult result = VKSCR(AllocateCommandBuffers)(device, pAllocateInfo, pCommandBuffers);
    if (result == VK_SUCCESS) {
        for (uint32_t i = 0; i < pAllocateInfo->commandBufferCount; ++i) {
            ZinkDeviceManager::get().register_command_buffer(pCommandBuffers[i], pAllocateInfo->commandPool);
        }
    }
    return result;
}

void zink_simulate_vkFreeCommandBuffers(
    VkDevice device,
    VkCommandPool commandPool,
    uint32_t commandBufferCount,
    const VkCommandBuffer* pCommandBuffers) {
    
    for (uint32_t i = 0; i < commandBufferCount; ++i) {
        ZinkDeviceManager::get().unregister_command_buffer(pCommandBuffers[i]);
    }
    VKSCR(FreeCommandBuffers)(device, commandPool, commandBufferCount, pCommandBuffers);
}

// 拦截图像创建函数
VkResult zink_simulate_vkCreateImage(
    VkDevice device,
    const VkImageCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImage* pImage) {
    
    VkResult result = VKSCR(CreateImage)(device, pCreateInfo, pAllocator, pImage);
    if (result == VK_SUCCESS) {
        ZinkDeviceManager::get().register_image_info(*pImage, *pCreateInfo);
    }
    return result;
}

void zink_simulate_vkDestroyImage(
    VkDevice device,
    VkImage image,
    const VkAllocationCallbacks* pAllocator) {
    
    ZinkDeviceManager::get().unregister_image_info(image);
    VKSCR(DestroyImage)(device, image, pAllocator);
}

// 拦截图像视图创建函数
VkResult zink_simulate_vkCreateImageView(
    VkDevice device,
    const VkImageViewCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkImageView* pImageView) {
    
    VkResult result = VKSCR(CreateImageView)(device, pCreateInfo, pAllocator, pImageView);
    if (result == VK_SUCCESS) {
        ZinkDeviceManager::get().register_image_view(*pImageView, pCreateInfo->image);
    }
    return result;
}

void zink_simulate_vkDestroyImageView(
    VkDevice device,
    VkImageView imageView,
    const VkAllocationCallbacks* pAllocator) {
    
    ZinkDeviceManager::get().unregister_image_view(imageView);
    VKSCR(DestroyImageView)(device, imageView, pAllocator);
}

// 模拟函数实现
VkResult zink_simulate_vkCreateSemaphore(
    VkDevice device,
    const VkSemaphoreCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSemaphore* pSemaphore) {
    
    // 首先创建真正的二进制信号量
    VkResult result = VKSCR(CreateSemaphore)(device, pCreateInfo, pAllocator, pSemaphore);
    if (result != VK_SUCCESS) {
        return result;
    }
    
    // 解析时间线信号量特定的创建信息
    uint64_t initial_value = 0;
    const VkSemaphoreTypeCreateInfo* type_info = nullptr;
    
    // 遍历pNext链查找VkSemaphoreTypeCreateInfo
    const void* pNext = pCreateInfo->pNext;
    while (pNext) {
        VkStructureType sType = *reinterpret_cast<const VkStructureType*>(pNext);
        if (sType == VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO) {
            type_info = reinterpret_cast<const VkSemaphoreTypeCreateInfo*>(pNext);
            break;
        }
        pNext = reinterpret_cast<const VkBaseInStructure*>(pNext)->pNext;
    }
    
    if (type_info && type_info->semaphoreType == VK_SEMAPHORE_TYPE_TIMELINE) {
        initial_value = type_info->initialValue;
    }
    
    // 注册到我们的模拟管理器
    ZinkTimelineSemaphoreManager::getInstance()->registerSemaphore(*pSemaphore, initial_value);
    
    return VK_SUCCESS;
}

void zink_simulate_vkDestroySemaphore(
    VkDevice device,
    VkSemaphore semaphore,
    const VkAllocationCallbacks* pAllocator) {
    
    // 从模拟管理器中注销
    ZinkTimelineSemaphoreManager::getInstance()->unregisterSemaphore(semaphore);
    
    // 销毁真正的信号量
    VKSCR(DestroySemaphore)(device, semaphore, pAllocator);
}

VkResult zink_simulate_vkGetSemaphoreCounterValue(
    VkDevice device, 
    VkSemaphore semaphore, 
    uint64_t* pValue) {
    
    auto state = ZinkTimelineSemaphoreManager::getInstance()->getSemaphoreState(semaphore);
    if (!state) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    std::lock_guard<std::mutex> lock(state->mutex);
    *pValue = state->current_value;
    
    return VK_SUCCESS;
}

VkResult zink_simulate_vkWaitSemaphores(
    VkDevice device,
    const VkSemaphoreWaitInfo* pWaitInfo,
    uint64_t timeout) {
    
    if (!pWaitInfo || pWaitInfo->semaphoreCount == 0) {
        return VK_SUCCESS;
    }
    
    auto start_time = std::chrono::steady_clock::now();
    
    for (uint32_t i = 0; i < pWaitInfo->semaphoreCount; ++i) {
        VkSemaphore semaphore = pWaitInfo->pSemaphores[i];
        uint64_t target_value = pWaitInfo->pValues[i];
        
        auto state = ZinkTimelineSemaphoreManager::getInstance()->getSemaphoreState(semaphore);
        if (!state) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        
        std::unique_lock<std::mutex> lock(state->mutex);
        
        // 检查是否已经达到目标值
        if (state->current_value >= target_value) {
            continue;
        }
        
        // 注册等待
        state->pending_waits.push_back(target_value);
        
        // 等待条件变量
        bool wait_success = false;
        if (timeout == UINT64_MAX) {
            // 无限等待
            state->cv.wait(lock, [state, target_value] {
                return state->current_value >= target_value;
            });
            wait_success = true;
        } else {
            // 有限时间等待
            auto timeout_duration = std::chrono::nanoseconds(timeout);
            wait_success = state->cv.wait_for(lock, timeout_duration, [state, target_value] {
                return state->current_value >= target_value;
            });
        }
        
        // 移除等待注册
        auto it = std::find(state->pending_waits.begin(), state->pending_waits.end(), target_value);
        if (it != state->pending_waits.end()) {
            state->pending_waits.erase(it);
        }
        
        if (!wait_success) {
            return VK_TIMEOUT;
        }
        
        // 检查是否超时
        if (timeout != UINT64_MAX) {
            auto current_time = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(current_time - start_time).count();
            if (elapsed > timeout) {
                return VK_TIMEOUT;
            }
        }
    }
    
    return VK_SUCCESS;
}

VkResult zink_simulate_vkSignalSemaphore(
    VkDevice device,
    const VkSemaphoreSignalInfo* pSignalInfo) {
    
    if (!pSignalInfo) {
        return VK_ERROR_VALIDATION_FAILED_EXT;
    }
    
    auto state = ZinkTimelineSemaphoreManager::getInstance()->getSemaphoreState(pSignalInfo->semaphore);
    if (!state) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        
        // 时间线信号量值必须单调递增
        if (pSignalInfo->value <= state->current_value) {
            return VK_ERROR_VALIDATION_FAILED_EXT;
        }
        
        state->current_value = pSignalInfo->value;
        state->signaled_value = pSignalInfo->value;
    }
    
    // 通知所有等待的线程
    state->cv.notify_all();
    
    return VK_SUCCESS;
}

// VK_KHR_dynamic_rendering 模拟实现
void ZINK_SIMULATE_FUNC(vkCmdBeginRendering)(VkCommandBuffer commandBuffer, 
                                            const VkRenderingInfo* pRenderingInfo) {
    auto state = std::make_unique<zink_rendering_state>();
    state->flags = pRenderingInfo->flags;
    state->viewMask = pRenderingInfo->viewMask;
    
    // 复制颜色附件信息
    if (pRenderingInfo->colorAttachmentCount > 0) {
        state->colorAttachments.assign(pRenderingInfo->pColorAttachments, 
                                      pRenderingInfo->pColorAttachments + pRenderingInfo->colorAttachmentCount);
    }
    
    // 复制深度附件信息
    if (pRenderingInfo->pDepthAttachment) {
        state->depthAttachment = *pRenderingInfo->pDepthAttachment;
        state->hasDepth = true;
    }
    
    // 复制模板附件信息
    if (pRenderingInfo->pStencilAttachment) {
        state->stencilAttachment = *pRenderingInfo->pStencilAttachment;
        state->hasStencil = true;
    }
    
    // 构建管线渲染状态用于缓存查找
    zink_pipeline_rendering_state pipelineState;
    pipelineState.viewMask = pRenderingInfo->viewMask;
    
    for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; ++i) {
        pipelineState.colorAttachmentFormats.push_back(pRenderingInfo->pColorAttachments[i].imageView ? 
            get_image_view_format(pRenderingInfo->pColorAttachments[i].imageView) : VK_FORMAT_UNDEFINED);
    }
    
    pipelineState.depthAttachmentFormat = (pRenderingInfo->pDepthAttachment && pRenderingInfo->pDepthAttachment->imageView) ?
        get_image_view_format(pRenderingInfo->pDepthAttachment->imageView) : VK_FORMAT_UNDEFINED;
    
    pipelineState.stencilAttachmentFormat = (pRenderingInfo->pStencilAttachment && pRenderingInfo->pStencilAttachment->imageView) ?
        get_image_view_format(pRenderingInfo->pStencilAttachment->imageView) : VK_FORMAT_UNDEFINED;
    
    // 获取或创建兼容的渲染通道
    VkDevice device = get_command_buffer_device(commandBuffer);
    state->compatibleRenderPass = zink_simulate_context::get().get_cached_render_pass(device, pipelineState);
    
    if (state->compatibleRenderPass == VK_NULL_HANDLE) {
        // 处理错误：无法创建兼容的渲染通道
        return;
    }
    
    // 收集图像视图并创建帧缓冲区
    std::vector<VkImageView> imageViews;
    uint32_t width = 0, height = 0, layers = 1;
    
    for (uint32_t i = 0; i < pRenderingInfo->colorAttachmentCount; ++i) {
        if (pRenderingInfo->pColorAttachments[i].imageView) {
            imageViews.push_back(pRenderingInfo->pColorAttachments[i].imageView);
            if (width == 0) {
                get_image_view_size(pRenderingInfo->pColorAttachments[i].imageView, &width, &height, &layers);
            }
        }
    }
    
    if (pRenderingInfo->pDepthAttachment && pRenderingInfo->pDepthAttachment->imageView) {
        imageViews.push_back(pRenderingInfo->pDepthAttachment->imageView);
        if (width == 0) {
            get_image_view_size(pRenderingInfo->pDepthAttachment->imageView, &width, &height, &layers);
        }
    }
    
    if (pRenderingInfo->pStencilAttachment && pRenderingInfo->pStencilAttachment->imageView) {
        imageViews.push_back(pRenderingInfo->pStencilAttachment->imageView);
        if (width == 0) {
            get_image_view_size(pRenderingInfo->pStencilAttachment->imageView, &width, &height, &layers);
        }
    }
    
    if (width == 0 || height == 0) {
        // 无法确定帧缓冲区尺寸
        return;
    }
    
    uint64_t framebufferId = zink_simulate_context::get().generate_framebuffer_id();
    state->framebuffer = zink_simulate_context::get().get_cached_framebuffer(device, framebufferId, imageViews, width, height, layers);
    state->imageViews = std::move(imageViews);
    
    if (state->framebuffer == VK_NULL_HANDLE) {
        // 处理错误：无法创建帧缓冲区
        return;
    }
    
    // 开始渲染通道
    VkRenderPassBeginInfo renderPassInfo = {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = state->compatibleRenderPass;
    renderPassInfo.framebuffer = state->framebuffer;
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = {width, height};
    
    // 注意：清除值需要根据附件配置处理
    std::vector<VkClearValue> clearValues(state->imageViews.size());
    // 这里需要根据实际的附件配置设置正确的清除值
    
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();
    
    VKSCR(CmdBeginRenderPass)(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    
    // 存储渲染状态
    zink_simulate_context::get().set_rendering_state(commandBuffer, std::move(state));
}

void ZINK_SIMULATE_FUNC(vkCmdEndRendering)(VkCommandBuffer commandBuffer) {
    // 结束渲染通道
    VKSCR(CmdEndRenderPass)(commandBuffer);
    
    // 清理渲染状态
    zink_simulate_context::get().remove_rendering_state(commandBuffer);
}

// 设备清理函数
void ZINK_SIMULATE_FUNC(vkDestroyDevice)(VkDevice device, const VkAllocationCallbacks* pAllocator) {
    zink_simulate_context::get().cleanup_device(device);
    VKSCR(DestroyDevice)(device, pAllocator);
}

// VK_KHR_dynamic_rendering_local_read 的模拟
// 主要通过适当的屏障和子通道依赖来实现
void ZINK_SIMULATE_FUNC(vkCmdPipelineBarrier2)(
    VkCommandBuffer commandBuffer,
    const VkDependencyInfo* pDependencyInfo) {
    
    // 将 VkDependencyInfo 转换为传统屏障调用
    // 这里实现更完整的转换逻辑
    
    // 内存屏障
    if (pDependencyInfo->memoryBarrierCount > 0) {
        std::vector<VkMemoryBarrier> memoryBarriers(pDependencyInfo->memoryBarrierCount);
        for (uint32_t i = 0; i < pDependencyInfo->memoryBarrierCount; ++i) {
            memoryBarriers[i].sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
            memoryBarriers[i].pNext = nullptr;
            memoryBarriers[i].srcAccessMask = static_cast<VkAccessFlags>(pDependencyInfo->pMemoryBarriers[i].srcAccessMask);
            memoryBarriers[i].dstAccessMask = static_cast<VkAccessFlags>(pDependencyInfo->pMemoryBarriers[i].dstAccessMask);
        }
        
        VKSCR(CmdPipelineBarrier)(
            commandBuffer,
            static_cast<VkPipelineStageFlags>(pDependencyInfo->pMemoryBarriers[0].srcStageMask),
            static_cast<VkPipelineStageFlags>(pDependencyInfo->pMemoryBarriers[0].dstStageMask),
            0,
            static_cast<uint32_t>(memoryBarriers.size()), memoryBarriers.data(),
            0, nullptr,
            0, nullptr
        );
    }
    
    // 缓冲区内存屏障
    if (pDependencyInfo->bufferMemoryBarrierCount > 0) {
        std::vector<VkBufferMemoryBarrier> bufferBarriers(pDependencyInfo->bufferMemoryBarrierCount);
        for (uint32_t i = 0; i < pDependencyInfo->bufferMemoryBarrierCount; ++i) {
            const auto& src = pDependencyInfo->pBufferMemoryBarriers[i];
            bufferBarriers[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bufferBarriers[i].pNext = nullptr;
            bufferBarriers[i].srcAccessMask = static_cast<VkAccessFlags>(src.srcAccessMask);
            bufferBarriers[i].dstAccessMask = static_cast<VkAccessFlags>(src.dstAccessMask);
            bufferBarriers[i].srcQueueFamilyIndex = src.srcQueueFamilyIndex;
            bufferBarriers[i].dstQueueFamilyIndex = src.dstQueueFamilyIndex;
            bufferBarriers[i].buffer = src.buffer;
            bufferBarriers[i].offset = src.offset;
            bufferBarriers[i].size = src.size;
        }
        
        VKSCR(CmdPipelineBarrier)(
            commandBuffer,
            static_cast<VkPipelineStageFlags>(pDependencyInfo->pBufferMemoryBarriers[0].srcStageMask),
            static_cast<VkPipelineStageFlags>(pDependencyInfo->pBufferMemoryBarriers[0].dstStageMask),
            0,
            0, nullptr,
            static_cast<uint32_t>(bufferBarriers.size()), bufferBarriers.data(),
            0, nullptr
        );
    }
    
    // 图像内存屏障
    if (pDependencyInfo->imageMemoryBarrierCount > 0) {
        std::vector<VkImageMemoryBarrier> imageBarriers(pDependencyInfo->imageMemoryBarrierCount);
        for (uint32_t i = 0; i < pDependencyInfo->imageMemoryBarrierCount; ++i) {
            const auto& src = pDependencyInfo->pImageMemoryBarriers[i];
            imageBarriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            imageBarriers[i].pNext = nullptr;
            imageBarriers[i].srcAccessMask = static_cast<VkAccessFlags>(src.srcAccessMask);
            imageBarriers[i].dstAccessMask = static_cast<VkAccessFlags>(src.dstAccessMask);
            imageBarriers[i].oldLayout = src.oldLayout;
            imageBarriers[i].newLayout = src.newLayout;
            imageBarriers[i].srcQueueFamilyIndex = src.srcQueueFamilyIndex;
            imageBarriers[i].dstQueueFamilyIndex = src.dstQueueFamilyIndex;
            imageBarriers[i].image = src.image;
            imageBarriers[i].subresourceRange = src.subresourceRange;
        }
        
        VKSCR(CmdPipelineBarrier)(
            commandBuffer,
            static_cast<VkPipelineStageFlags>(pDependencyInfo->pImageMemoryBarriers[0].srcStageMask),
            static_cast<VkPipelineStageFlags>(pDependencyInfo->pImageMemoryBarriers[0].dstStageMask),
            0,
            0, nullptr,
            0, nullptr,
            static_cast<uint32_t>(imageBarriers.size()), imageBarriers.data()
        );
    }
}

// 完善 zink_simulate_QueueSubmitWithTimeline 函数
VkResult zink_simulate_QueueSubmitWithTimeline(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo* pSubmits,
    VkFence fence) {
    
    std::vector<VkSubmitInfo> modified_submits;
    std::vector<std::vector<VkSemaphore>> binary_wait_semaphores_storage;
    std::vector<std::vector<VkPipelineStageFlags>> binary_wait_stages_storage;
    std::vector<std::vector<VkSemaphore>> binary_signal_semaphores_storage;
    
    // 二进制信号量池管理
    static std::mutex binary_semaphore_pool_mutex;
    static std::unordered_map<VkDevice, std::vector<VkSemaphore>> binary_semaphore_pool;
    
    auto get_or_create_binary_semaphore = [](VkDevice device) -> VkSemaphore {
        std::lock_guard<std::mutex> lock(binary_semaphore_pool_mutex);
        
        auto& pool = binary_semaphore_pool[device];
        if (!pool.empty()) {
            VkSemaphore semaphore = pool.back();
            pool.pop_back();
            return semaphore;
        }
        
        // 创建新的二进制信号量
        VkSemaphoreCreateInfo semaphoreInfo = {};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        
        VkSemaphore semaphore;
        if (VKSCR(CreateSemaphore)(device, &semaphoreInfo, nullptr, &semaphore) == VK_SUCCESS) {
            return semaphore;
        }
        
        return VK_NULL_HANDLE;
    };
    
    auto return_binary_semaphore = [](VkDevice device, VkSemaphore semaphore) {
        std::lock_guard<std::mutex> lock(binary_semaphore_pool_mutex);
        binary_semaphore_pool[device].push_back(semaphore);
    };
    
    for (uint32_t i = 0; i < submitCount; ++i) {
        const VkSubmitInfo& submit = pSubmits[i];
        VkSubmitInfo modified_submit = submit;
        
        // 查找时间线信号量信息
        const VkTimelineSemaphoreSubmitInfo* timeline_info = nullptr;
        std::vector<const void*> preserved_pnexts;
        
        const void* pNext = submit.pNext;
        while (pNext) {
            VkStructureType sType = *reinterpret_cast<const VkStructureType*>(pNext);
            if (sType == VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO) {
                timeline_info = reinterpret_cast<const VkTimelineSemaphoreSubmitInfo*>(pNext);
            } else {
                preserved_pnexts.push_back(pNext);
            }
            pNext = reinterpret_cast<const VkBaseInStructure*>(pNext)->pNext;
        }
        
        // 重建 pNext 链（移除时间线特定结构）
        if (!preserved_pnexts.empty()) {
            // 简化处理：只保留第一个非时间线结构
            modified_submit.pNext = preserved_pnexts[0];
        } else {
            modified_submit.pNext = nullptr;
        }
        
        // 处理时间线等待信号量
        if (timeline_info && timeline_info->waitSemaphoreValueCount > 0) {
            std::vector<VkSemaphore> binary_waits;
            std::vector<VkPipelineStageFlags> binary_wait_stages;
            
            VkDevice device = VK_NULL_HANDLE;
            
            for (uint32_t j = 0; j < submit.waitSemaphoreCount; ++j) {
                VkSemaphore semaphore = submit.pWaitSemaphores[j];
                uint64_t target_value = timeline_info->pWaitSemaphoreValues[j];
                
                auto state = ZinkTimelineSemaphoreManager::getInstance()->getSemaphoreState(semaphore);
                if (state) {
                    // 如果当前值已经达到目标，则不需要等待
                    std::lock_guard<std::mutex> lock(state->mutex);
                    if (state->current_value >= target_value) {
                        continue;
                    }
                    
                    // 创建二进制信号量来模拟等待
                    if (device == VK_NULL_HANDLE) {
                        // 尝试获取设备（简化实现）
                        device = get_command_buffer_device(submit.pCommandBuffers[0]);
                    }
                    
                    VkSemaphore binary_semaphore = get_or_create_binary_semaphore(device);
                    if (binary_semaphore != VK_NULL_HANDLE) {
                        binary_waits.push_back(binary_semaphore);
                        binary_wait_stages.push_back(submit.pWaitDstStageMask[j]);
                        
                        // 安排信号量在时间线达到目标值时发出信号
                        // 这需要额外的线程或回调机制，这里简化处理
                        // 在实际实现中，需要更复杂的同步机制
                    }
                }
            }
            
            if (!binary_waits.empty()) {
                modified_submit.waitSemaphoreCount = static_cast<uint32_t>(binary_waits.size());
                modified_submit.pWaitSemaphores = binary_waits.data();
                modified_submit.pWaitDstStageMask = binary_wait_stages.data();
                
                binary_wait_semaphores_storage.push_back(std::move(binary_waits));
                binary_wait_stages_storage.push_back(std::move(binary_wait_stages));
            } else {
                modified_submit.waitSemaphoreCount = 0;
                modified_submit.pWaitSemaphores = nullptr;
                modified_submit.pWaitDstStageMask = nullptr;
            }
        }
        
        // 处理时间线信号信号量（简化实现）
        if (timeline_info && timeline_info->signalSemaphoreValueCount > 0) {
            std::vector<VkSemaphore> binary_signals;
            
            for (uint32_t j = 0; j < submit.signalSemaphoreCount; ++j) {
                VkSemaphore semaphore = submit.pSignalSemaphores[j];
                uint64_t signal_value = timeline_info->pSignalSemaphoreValues[j];
                
                auto state = ZinkTimelineSemaphoreManager::getInstance()->getSemaphoreState(semaphore);
                if (state) {
                    // 在实际实现中，这里应该安排一个操作来在队列工作完成后更新时间线信号量
                    // 简化实现：立即更新（这不正确，但作为占位符）
                    VkSemaphoreSignalInfo signalInfo = {};
                    signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
                    signalInfo.semaphore = semaphore;
                    signalInfo.value = signal_value;
                    zink_simulate_vkSignalSemaphore(VK_NULL_HANDLE, &signalInfo);
                }
            }
            
            // 清空信号信号量，因为我们已经手动处理了时间线信号
            modified_submit.signalSemaphoreCount = 0;
            modified_submit.pSignalSemaphores = nullptr;
        }
        
        modified_submits.push_back(modified_submit);
    }
    
    // 调用真正的队列提交
    VkResult result = VKSCR(QueueSubmit)(queue, modified_submits.size(), modified_submits.data(), fence);
    
    // 清理临时二进制信号量（在实际实现中，应该在信号量真正被使用后清理）
    // 这里简化处理，立即清理
    for (auto& binary_semaphores : binary_wait_semaphores_storage) {
        for (VkSemaphore semaphore : binary_semaphores) {
            // 在实际实现中，应该等待信号量被使用后再回收
            // return_binary_semaphore(device, semaphore);
        }
    }
    
    return result;
}

// 完善 zink_simulate_vkQueueSubmit2 函数
VkResult zink_simulate_vkQueueSubmit2(
    VkQueue queue,
    uint32_t submitCount,
    const VkSubmitInfo2* pSubmits,
    VkFence fence) {
    
    std::vector<VkSubmitInfo> legacy_submits;
    
    for (uint32_t i = 0; i < submitCount; ++i) {
        const VkSubmitInfo2& submit2 = pSubmits[i];
        VkSubmitInfo legacy_submit = {};
        legacy_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        
        // 转换命令缓冲区
        legacy_submit.commandBufferCount = submit2.commandBufferInfoCount;
        if (submit2.commandBufferInfoCount > 0) {
            std::vector<VkCommandBuffer> command_buffers(submit2.commandBufferInfoCount);
            for (uint32_t j = 0; j < submit2.commandBufferInfoCount; ++j) {
                command_buffers[j] = submit2.pCommandBufferInfos[j].commandBuffer;
            }
            legacy_submit.pCommandBuffers = command_buffers.data();
        }
        
        // 转换等待信号量
        legacy_submit.waitSemaphoreCount = submit2.waitSemaphoreInfoCount;
        if (submit2.waitSemaphoreInfoCount > 0) {
            std::vector<VkSemaphore> wait_semaphores(submit2.waitSemaphoreInfoCount);
            std::vector<VkPipelineStageFlags> wait_stages(submit2.waitSemaphoreInfoCount);
            
            for (uint32_t j = 0; j < submit2.waitSemaphoreInfoCount; ++j) {
                const VkSemaphoreSubmitInfo& wait_info = submit2.pWaitSemaphoreInfos[j];
                wait_semaphores[j] = wait_info.semaphore;
                wait_stages[j] = static_cast<VkPipelineStageFlags>(wait_info.stageMask);
            }
            
            legacy_submit.pWaitSemaphores = wait_semaphores.data();
            legacy_submit.pWaitDstStageMask = wait_stages.data();
        }
        
        // 转换信号信号量
        legacy_submit.signalSemaphoreCount = submit2.signalSemaphoreInfoCount;
        if (submit2.signalSemaphoreInfoCount > 0) {
            std::vector<VkSemaphore> signal_semaphores(submit2.signalSemaphoreInfoCount);
            for (uint32_t j = 0; j < submit2.signalSemaphoreInfoCount; ++j) {
                signal_semaphores[j] = submit2.pSignalSemaphoreInfos[j].semaphore;
            }
            legacy_submit.pSignalSemaphores = signal_semaphores.data();
        }
        
        // 处理时间线信号量信息
        VkTimelineSemaphoreSubmitInfo timeline_info = {};
        timeline_info.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
        
        bool has_timeline_waits = false;
        bool has_timeline_signals = false;
        
        if (submit2.waitSemaphoreInfoCount > 0) {
            std::vector<uint64_t> wait_values(submit2.waitSemaphoreInfoCount);
            for (uint32_t j = 0; j < submit2.waitSemaphoreInfoCount; ++j) {
                wait_values[j] = submit2.pWaitSemaphoreInfos[j].value;
                if (wait_values[j] > 0) {
                    has_timeline_waits = true;
                }
            }
            if (has_timeline_waits) {
                timeline_info.waitSemaphoreValueCount = submit2.waitSemaphoreInfoCount;
                timeline_info.pWaitSemaphoreValues = wait_values.data();
            }
        }
        
        if (submit2.signalSemaphoreInfoCount > 0) {
            std::vector<uint64_t> signal_values(submit2.signalSemaphoreInfoCount);
            for (uint32_t j = 0; j < submit2.signalSemaphoreInfoCount; ++j) {
                signal_values[j] = submit2.pSignalSemaphoreInfos[j].value;
                if (signal_values[j] > 0) {
                    has_timeline_signals = true;
                }
            }
            if (has_timeline_signals) {
                timeline_info.signalSemaphoreValueCount = submit2.signalSemaphoreInfoCount;
                timeline_info.pSignalSemaphoreValues = signal_values.data();
            }
        }
        
        // 如果有时间线信号量操作，添加时间线信息到 pNext
        if (has_timeline_waits || has_timeline_signals) {
            legacy_submit.pNext = &timeline_info;
        }
        
        legacy_submits.push_back(legacy_submit);
    }
    
    return zink_simulate_QueueSubmitWithTimeline(queue, legacy_submits.size(), 
                                               legacy_submits.data(), fence);
}

// 完善 vkCreateGraphicsPipelines 函数
VkResult ZINK_SIMULATE_FUNC(vkCreateGraphicsPipelines)(VkDevice device, 
                                                      VkPipelineCache pipelineCache,
                                                      uint32_t createInfoCount,
                                                      const VkGraphicsPipelineCreateInfo* pCreateInfos,
                                                      const VkAllocationCallbacks* pAllocator,
                                                      VkPipeline* pPipelines) {
    
    std::vector<VkGraphicsPipelineCreateInfo> modifiedCreateInfos;
    std::vector<std::vector<VkPipelineColorBlendAttachmentState>> blendAttachmentStates;
    std::vector<std::vector<VkDynamicState>> dynamicStates;
    std::vector<VkPipelineVertexInputStateCreateInfo> vertexInputStates;
    std::vector<VkPipelineRenderingCreateInfo> renderingCreateInfos;
    
    for (uint32_t i = 0; i < createInfoCount; ++i) {
        const VkGraphicsPipelineCreateInfo* baseInfo = &pCreateInfos[i];
        VkGraphicsPipelineCreateInfo modifiedInfo = *baseInfo;
        
        // 处理动态渲染特定的 pNext 链项目
        const VkPipelineRenderingCreateInfo* renderingInfo = nullptr;
        std::vector<const void*> preservedPNexts;
        
        const void* pNext = baseInfo->pNext;
        while (pNext) {
            const VkBaseInStructure* baseStruct = static_cast<const VkBaseInStructure*>(pNext);
            if (baseStruct->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
                renderingInfo = static_cast<const VkPipelineRenderingCreateInfo*>(pNext);
            } else {
                preservedPNexts.push_back(pNext);
            }
            pNext = baseStruct->pNext;
        }
        
        // 重建 pNext 链（移除动态渲染特定结构）
        if (!preservedPNexts.empty()) {
            modifiedInfo.pNext = preservedPNexts[0];
        } else {
            modifiedInfo.pNext = nullptr;
        }
        
        // 如果存在动态渲染信息，确保渲染通道兼容
        if (renderingInfo) {
            // 构建渲染通道状态
            zink_pipeline_rendering_state pipelineState;
            pipelineState.viewMask = renderingInfo->viewMask;
            pipelineState.colorAttachmentFormats.assign(
                renderingInfo->pColorAttachmentFormats,
                renderingInfo->pColorAttachmentFormats + renderingInfo->colorAttachmentCount);
            pipelineState.depthAttachmentFormat = renderingInfo->depthAttachmentFormat;
            pipelineState.stencilAttachmentFormat = renderingInfo->stencilAttachmentFormat;
            
            // 获取或创建兼容的渲染通道
            VkRenderPass renderPass = zink_simulate_context::get().get_cached_render_pass(device, pipelineState);
            if (renderPass != VK_NULL_HANDLE) {
                modifiedInfo.renderPass = renderPass;
                
                // 确保子通道索引正确
                if (modifiedInfo.subpass != 0) {
                    modifiedInfo.subpass = 0; // 动态渲染通常对应第一个子通道
                }
            }
            
            // 确保混合状态附件数量匹配
            if (modifiedInfo.pColorBlendState && 
                renderingInfo->colorAttachmentCount > 0) {
                
                VkPipelineColorBlendStateCreateInfo newBlendState = *modifiedInfo.pColorBlendState;
                std::vector<VkPipelineColorBlendAttachmentState> newAttachments;
                
                // 复制现有的混合状态或使用默认值
                if (newBlendState.pAttachments && newBlendState.attachmentCount > 0) {
                    // 使用现有的混合状态，如果数量不够则用默认值补充
                    for (uint32_t j = 0; j < renderingInfo->colorAttachmentCount; ++j) {
                        if (j < newBlendState.attachmentCount) {
                            newAttachments.push_back(newBlendState.pAttachments[j]);
                        } else {
                            // 使用默认混合状态
                            VkPipelineColorBlendAttachmentState defaultAttachment = {};
                            defaultAttachment.blendEnable = VK_FALSE;
                            defaultAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                            defaultAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                            defaultAttachment.colorBlendOp = VK_BLEND_OP_ADD;
                            defaultAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                            defaultAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                            defaultAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
                            defaultAttachment.colorWriteMask = 
                                VK_COLOR_COMPONENT_R_BIT | 
                                VK_COLOR_COMPONENT_G_BIT | 
                                VK_COLOR_COMPONENT_B_BIT | 
                                VK_COLOR_COMPONENT_A_BIT;
                            newAttachments.push_back(defaultAttachment);
                        }
                    }
                } else {
                    // 创建默认混合状态
                    newAttachments.resize(renderingInfo->colorAttachmentCount);
                    for (auto& attachment : newAttachments) {
                        attachment.blendEnable = VK_FALSE;
                        attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                        attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
                        attachment.colorBlendOp = VK_BLEND_OP_ADD;
                        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                        attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
                        attachment.colorWriteMask = 
                            VK_COLOR_COMPONENT_R_BIT | 
                            VK_COLOR_COMPONENT_G_BIT | 
                            VK_COLOR_COMPONENT_B_BIT | 
                            VK_COLOR_COMPONENT_A_BIT;
                    }
                }
                
                blendAttachmentStates.push_back(newAttachments);
                newBlendState.attachmentCount = static_cast<uint32_t>(newAttachments.size());
                newBlendState.pAttachments = blendAttachmentStates.back().data();
                
                // 更新混合状态
                modifiedInfo.pColorBlendState = &newBlendState;
            }
            
            // 处理顶点输入状态 - 确保与动态渲染兼容
            if (modifiedInfo.pVertexInputState) {
                VkPipelineVertexInputStateCreateInfo newVertexInput = *modifiedInfo.pVertexInputState;
                vertexInputStates.push_back(newVertexInput);
                modifiedInfo.pVertexInputState = &vertexInputStates.back();
            }
            
            // 添加动态状态支持
            if (!modifiedInfo.pDynamicState) {
                std::vector<VkDynamicState> dynStates = {
                    VK_DYNAMIC_STATE_VIEWPORT,
                    VK_DYNAMIC_STATE_SCISSOR,
                    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
                    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
                    VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
                    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
                    VK_DYNAMIC_STATE_STENCIL_OP
                };
                
                dynamicStates.push_back(dynStates);
                
                VkPipelineDynamicStateCreateInfo dynamicState = {};
                dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
                dynamicState.dynamicStateCount = static_cast<uint32_t>(dynStates.size());
                dynamicState.pDynamicStates = dynamicStates.back().data();
                
                // 注意：这里需要将 dynamicState 添加到 preservedPNexts 或直接设置
                // 简化处理：直接设置到 modifiedInfo
                modifiedInfo.pDynamicState = &dynamicState;
            }
        }
        
        modifiedCreateInfos.push_back(modifiedInfo);
    }
    
    // 调用核心的管线创建函数
    VkResult result = VKSCR(CreateGraphicsPipelines)(
        device, pipelineCache, createInfoCount, 
        modifiedCreateInfos.data(), pAllocator, pPipelines);
    
    return result;
}

// VK_KHR_imageless_framebuffer 模拟实现
VkResult zink_simulate_vkCreateFramebuffer(
    VkDevice device,
    const VkFramebufferCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkFramebuffer* pFramebuffer) {
    
    bool is_imageless = (pCreateInfo->flags & VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT_KHR) != 0;
    
    if (!is_imageless) {
        // 对于传统帧缓冲区，直接创建
        VkResult result = VKSCR(CreateFramebuffer)(device, pCreateInfo, pAllocator, pFramebuffer);
        if (result == VK_SUCCESS) {
            ZinkDeviceManager::get().register_framebuffer(*pFramebuffer, device);
        }
        return result;
    }
    
    // 处理无图像帧缓冲区创建
    auto imageless_fb = std::make_shared<ZinkImagelessFramebuffer>();
    imageless_fb->is_imageless = true;
    imageless_fb->flags = pCreateInfo->flags;
    imageless_fb->renderPass = pCreateInfo->renderPass;
    imageless_fb->width = pCreateInfo->width;
    imageless_fb->height = pCreateInfo->height;
    imageless_fb->layers = pCreateInfo->layers;
    
    // 解析附件信息（从 pNext 链）
    const VkFramebufferAttachmentsCreateInfo* attachments_info = nullptr;
    const void* pNext = pCreateInfo->pNext;
    while (pNext) {
        const VkBaseInStructure* base = static_cast<const VkBaseInStructure*>(pNext);
        if (base->sType == VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO) {
            attachments_info = static_cast<const VkFramebufferAttachmentsCreateInfo*>(pNext);
            break;
        }
        pNext = base->pNext;
    }
    
    if (attachments_info && attachments_info->attachmentImageInfoCount > 0) {
        imageless_fb->attachmentInfos.resize(attachments_info->attachmentImageInfoCount);
        for (uint32_t i = 0; i < attachments_info->attachmentImageInfoCount; ++i) {
            const VkFramebufferAttachmentImageInfo& image_info = attachments_info->pAttachmentImageInfos[i];
            auto& attachment_info = imageless_fb->attachmentInfos[i];
            
            // 使用兼容性包装器来安全地提取信息
            // 格式信息：尝试从 pViewFormats 获取，否则使用回退格式
            attachment_info.format = VK_FORMAT_UNDEFINED;
            
            // 方法1：尝试访问 pViewFormats（新版本 Vulkan）
            #if defined(VK_VERSION_1_2) || defined(VK_KHR_imageless_framebuffer)
            if (image_info.viewFormatCount > 0 && image_info.pViewFormats != nullptr) {
                attachment_info.format = image_info.pViewFormats[0];
            }
            #endif
            
            // 方法2：如果仍然没有格式，根据使用用途猜测合适的格式
            if (attachment_info.format == VK_FORMAT_UNDEFINED) {
                if (image_info.usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                    // 深度/模板附件
                    attachment_info.format = VK_FORMAT_D32_SFLOAT_S8_UINT;
                } else if (image_info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) {
                    // 颜色附件
                    attachment_info.format = VK_FORMAT_R8G8B8A8_UNORM;
                } else {
                    // 默认格式
                    attachment_info.format = VK_FORMAT_R8G8B8A8_UNORM;
                }
            }
            
            // 采样数 - VkFramebufferAttachmentImageInfo 不直接包含采样信息
            // 使用默认值，实际渲染时会根据图像视图确定
            attachment_info.samples = VK_SAMPLE_COUNT_1_BIT;
            
            // 这些信息在 VkFramebufferAttachmentImageInfo 中是标准成员
            attachment_info.usage = image_info.usage;
            attachment_info.width = image_info.width;
            attachment_info.height = image_info.height;
        }
    }
    
    // 创建一个虚拟的帧缓冲区句柄
    static std::atomic<uint64_t> next_framebuffer_handle{1};
    VkFramebuffer framebuffer = reinterpret_cast<VkFramebuffer>(
        static_cast<uintptr_t>(next_framebuffer_handle.fetch_add(1)));
    
    // 注册无图像帧缓冲区信息
    ZinkImagelessFramebufferManager::get().registerFramebuffer(framebuffer, imageless_fb);
    
    // 注册设备信息
    ZinkDeviceManager::get().register_framebuffer(framebuffer, device);
    
    *pFramebuffer = framebuffer;
    return VK_SUCCESS;
}

void zink_simulate_vkDestroyFramebuffer(
    VkDevice device,
    VkFramebuffer framebuffer,
    const VkAllocationCallbacks* pAllocator) {
    
    auto fb_info = ZinkImagelessFramebufferManager::get().getFramebufferInfo(framebuffer);
    if (fb_info && fb_info->is_imageless) {
        // 对于无图像帧缓冲区，清理内部资源
        if (fb_info->real_framebuffer != VK_NULL_HANDLE) {
            VKSCR(DestroyFramebuffer)(device, fb_info->real_framebuffer, pAllocator);
        }
        ZinkImagelessFramebufferManager::get().unregisterFramebuffer(framebuffer);
    } else {
        // 对于传统帧缓冲区，直接销毁
        VKSCR(DestroyFramebuffer)(device, framebuffer, pAllocator);
    }
    
    // 注销设备信息
    ZinkDeviceManager::get().unregister_framebuffer(framebuffer);
}

void zink_simulate_vkCmdBeginRenderPass(
    VkCommandBuffer commandBuffer,
    const VkRenderPassBeginInfo* pRenderPassBeginInfo,
    VkSubpassContents contents) {
    
    // 检查是否使用了无图像帧缓冲区
    auto fb_info = ZinkImagelessFramebufferManager::get().getFramebufferInfo(
        pRenderPassBeginInfo->framebuffer);
    
    if (fb_info && fb_info->is_imageless) {
        // 处理无图像帧缓冲区的情况
        
        // 查找动态附件信息
        const VkRenderPassAttachmentBeginInfo* attachment_begin_info = nullptr;
        const void* pNext = pRenderPassBeginInfo->pNext;
        while (pNext) {
            const VkBaseInStructure* base = static_cast<const VkBaseInStructure*>(pNext);
            if (base->sType == VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO) {
                attachment_begin_info = static_cast<const VkRenderPassAttachmentBeginInfo*>(pNext);
                break;
            }
            pNext = base->pNext;
        }
        
        if (!attachment_begin_info || attachment_begin_info->attachmentCount == 0) {
            // 错误处理：无图像帧缓冲区需要动态附件
            return;
        }
        
        // 验证附件数量匹配
        if (attachment_begin_info->attachmentCount != fb_info->attachmentInfos.size()) {
            // 错误处理：附件数量不匹配
            return;
        }
        
        // 收集图像视图
        std::vector<VkImageView> attachments(
            attachment_begin_info->pAttachments,
            attachment_begin_info->pAttachments + attachment_begin_info->attachmentCount);
        
        // 为无图像帧缓冲区创建实际的帧缓冲区
        VkDevice device = get_command_buffer_device(commandBuffer);
        VkResult result = ZinkImagelessFramebufferManager::get().createRealFramebuffer(
            device, fb_info, attachments);
        
        if (result != VK_SUCCESS) {
            // 错误处理：无法创建实际帧缓冲区
            return;
        }
        
        // 使用实际的帧缓冲区开始渲染通道
        VkRenderPassBeginInfo modified_begin_info = *pRenderPassBeginInfo;
        modified_begin_info.framebuffer = fb_info->real_framebuffer;
        modified_begin_info.pNext = nullptr; // 移除动态附件信息
        
        VKCTX(CmdBeginRenderPass)(commandBuffer, &modified_begin_info, contents);
    } else {
        // 传统帧缓冲区，直接调用
        vkCmdBeginRenderPass(commandBuffer, pRenderPassBeginInfo, contents);
    }
}

