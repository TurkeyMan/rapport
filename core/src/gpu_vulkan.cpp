// rap::gpu on Vulkan 1.3 (dynamic rendering, push descriptors, a timeline
// semaphore). The loader is opened at run time, so a machine without
// Vulkan still runs (with no device); lavapipe, Mesa's CPU Vulkan, is the
// device of last resort.
//
// Every image rests in SHADER_READ_ONLY_OPTIMAL: an upload, a pass's target
// level or a readback moves the level it touches out and back within its own
// commands. One queue, one command pool, under one lock; each submission
// signals the timeline, and a thread per device waits on it to finish
// readbacks and free what the work held (command buffers, staging, the
// app's textures and programs).
//
// Memory is one allocation per image or buffer for now; sub-allocation (VMA)
// is for when there are many small ones.
#include "gpu_internal.h"

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <dlfcn.h>

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#include <sys/stat.h>
#include <sys/sysmacros.h>

namespace rap::gpu
{
namespace
{

// rapport's vertex stage: one triangle over the whole target, from
// SV_VertexID (the HLSL in gpu_d3d11.cpp), DXC -spirv -T vs_6_0.
const uint32_t k_fullscreen_vs[] = {
    0x07230203, 0x00010000, 0x000e0000, 0x00000022, 0x00000000, 0x00020011, 0x00000001, 0x0003000e,
    0x00000000, 0x00000001, 0x0007000f, 0x00000000, 0x00000001, 0x6e69616d, 0x00000000, 0x00000002,
    0x00000003, 0x00030003, 0x00000005, 0x00000258, 0x00040005, 0x00000001, 0x6e69616d, 0x00000000,
    0x00040047, 0x00000002, 0x0000000b, 0x0000002a, 0x00040047, 0x00000003, 0x0000000b, 0x00000000,
    0x00040015, 0x00000004, 0x00000020, 0x00000000, 0x0004002b, 0x00000004, 0x00000005, 0x00000001,
    0x0004002b, 0x00000004, 0x00000006, 0x00000002, 0x00030016, 0x00000007, 0x00000020, 0x0004002b,
    0x00000007, 0x00000008, 0x40000000, 0x0004002b, 0x00000007, 0x00000009, 0xc0000000, 0x00040017,
    0x0000000a, 0x00000007, 0x00000002, 0x0005002c, 0x0000000a, 0x0000000b, 0x00000008, 0x00000009,
    0x0004002b, 0x00000007, 0x0000000c, 0xbf800000, 0x0004002b, 0x00000007, 0x0000000d, 0x3f800000,
    0x0005002c, 0x0000000a, 0x0000000e, 0x0000000c, 0x0000000d, 0x0004002b, 0x00000007, 0x0000000f,
    0x00000000, 0x00040020, 0x00000010, 0x00000001, 0x00000004, 0x00040017, 0x00000011, 0x00000007,
    0x00000004, 0x00040020, 0x00000012, 0x00000003, 0x00000011, 0x00020013, 0x00000013, 0x00030021,
    0x00000014, 0x00000013, 0x0004003b, 0x00000010, 0x00000002, 0x00000001, 0x0004003b, 0x00000012,
    0x00000003, 0x00000003, 0x00050036, 0x00000013, 0x00000001, 0x00000000, 0x00000014, 0x000200f8,
    0x00000015, 0x0004003d, 0x00000004, 0x00000016, 0x00000002, 0x000500c4, 0x00000004, 0x00000017,
    0x00000016, 0x00000005, 0x000500c7, 0x00000004, 0x00000018, 0x00000017, 0x00000006, 0x00040070,
    0x00000007, 0x00000019, 0x00000018, 0x000500c7, 0x00000004, 0x0000001a, 0x00000016, 0x00000006,
    0x00040070, 0x00000007, 0x0000001b, 0x0000001a, 0x00050050, 0x0000000a, 0x0000001c, 0x00000019,
    0x0000001b, 0x00050085, 0x0000000a, 0x0000001d, 0x0000001c, 0x0000000b, 0x00050081, 0x0000000a,
    0x0000001e, 0x0000001d, 0x0000000e, 0x00050051, 0x00000007, 0x0000001f, 0x0000001e, 0x00000000,
    0x00050051, 0x00000007, 0x00000020, 0x0000001e, 0x00000001, 0x00070050, 0x00000011, 0x00000021,
    0x0000001f, 0x00000020, 0x0000000f, 0x0000000d, 0x0003003e, 0x00000003, 0x00000021, 0x000100fd,
    0x00010038,
};

#define RAP_VK_INSTANCE_FNS(X)                   \
    X(vkDestroyInstance)                         \
    X(vkEnumeratePhysicalDevices)                \
    X(vkGetPhysicalDeviceProperties2)            \
    X(vkGetPhysicalDeviceQueueFamilyProperties)  \
    X(vkGetPhysicalDeviceMemoryProperties)       \
    X(vkGetPhysicalDeviceMemoryProperties2)      \
    X(vkGetPhysicalDeviceFormatProperties)       \
    X(vkEnumerateDeviceExtensionProperties)      \
    X(vkCreateDevice)                            \
    X(vkGetDeviceProcAddr)

#define RAP_VK_DEVICE_FNS(X)          \
    X(vkDestroyDevice)                \
    X(vkGetDeviceQueue)               \
    X(vkDeviceWaitIdle)               \
    X(vkCreateCommandPool)            \
    X(vkDestroyCommandPool)           \
    X(vkAllocateCommandBuffers)       \
    X(vkFreeCommandBuffers)           \
    X(vkBeginCommandBuffer)           \
    X(vkEndCommandBuffer)             \
    X(vkQueueSubmit)                  \
    X(vkCreateSemaphore)              \
    X(vkDestroySemaphore)             \
    X(vkGetSemaphoreCounterValue)     \
    X(vkWaitSemaphores)               \
    X(vkCreateImage)                  \
    X(vkDestroyImage)                 \
    X(vkGetImageMemoryRequirements)   \
    X(vkBindImageMemory)              \
    X(vkCreateImageView)              \
    X(vkDestroyImageView)             \
    X(vkCreateBuffer)                 \
    X(vkDestroyBuffer)                \
    X(vkGetBufferMemoryRequirements)  \
    X(vkBindBufferMemory)             \
    X(vkAllocateMemory)               \
    X(vkFreeMemory)                   \
    X(vkMapMemory)                    \
    X(vkUnmapMemory)                  \
    X(vkCmdPipelineBarrier)           \
    X(vkCmdCopyBufferToImage)         \
    X(vkCmdCopyImageToBuffer)         \
    X(vkCreateSampler)                \
    X(vkDestroySampler)               \
    X(vkCreateShaderModule)           \
    X(vkDestroyShaderModule)          \
    X(vkCreateDescriptorSetLayout)    \
    X(vkDestroyDescriptorSetLayout)   \
    X(vkCreatePipelineLayout)         \
    X(vkDestroyPipelineLayout)        \
    X(vkCreateGraphicsPipelines)      \
    X(vkDestroyPipeline)              \
    X(vkCmdBeginRendering)            \
    X(vkCmdEndRendering)              \
    X(vkCmdBindPipeline)              \
    X(vkCmdPushDescriptorSetKHR)      \
    X(vkCmdSetViewport)               \
    X(vkCmdSetScissor)                \
    X(vkCmdDraw)

// The instance and device, and their functions: what every image, buffer
// and program holds on to, so they can be freed after the rap::gpu::device
// that made them is gone.
struct core
{
#define RAP_VK_POINTER(name) PFN_##name name = nullptr;
    RAP_VK_INSTANCE_FNS(RAP_VK_POINTER)
    RAP_VK_DEVICE_FNS(RAP_VK_POINTER)
#undef RAP_VK_POINTER
    void* library = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory{};
    VkPhysicalDeviceProperties properties{};
    bool budget_ext = false;

    ~core()
    {
        if (device) vkDestroyDevice(device, nullptr);
        if (instance) vkDestroyInstance(instance, nullptr);
        // The library stays loaded: drivers don't always unload cleanly.
    }

    // A memory type `bits` allows with `want` (else any of `bits`).
    uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) const
    {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & want) == want) return i;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if (bits & (1u << i)) return i;
        return UINT32_MAX;
    }
};
using core_ptr = std::shared_ptr<core>;

VkFormat vk_format(format f)
{
    switch (f)
    {
        case format::r8: return VK_FORMAT_R8_UNORM;
        case format::rg8: return VK_FORMAT_R8G8_UNORM;
        case format::rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
        case format::rgba8_srgb: return VK_FORMAT_R8G8B8A8_SRGB;
        case format::r16: return VK_FORMAT_R16_UNORM;
        case format::rg16: return VK_FORMAT_R16G16_UNORM;
        case format::rgba16: return VK_FORMAT_R16G16B16A16_UNORM;
        case format::r16_snorm: return VK_FORMAT_R16_SNORM;
        case format::rg16_snorm: return VK_FORMAT_R16G16_SNORM;
        case format::rgba16_snorm: return VK_FORMAT_R16G16B16A16_SNORM;
        case format::r16f: return VK_FORMAT_R16_SFLOAT;
        case format::rg16f: return VK_FORMAT_R16G16_SFLOAT;
        case format::rgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
        case format::r32f: return VK_FORMAT_R32_SFLOAT;
        case format::rg32f: return VK_FORMAT_R32G32_SFLOAT;
        case format::rgba32f: return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

// A buffer with host-visible memory, mapped: staging for uploads and
// readbacks, and uniform blocks.
struct host_buffer
{
    core_ptr c;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* mapped = nullptr;
    VkDeviceSize size = 0;

    static std::shared_ptr<host_buffer> make(const core_ptr& c, VkDeviceSize size, VkBufferUsageFlags usage)
    {
        auto b = std::make_shared<host_buffer>();
        b->c = c;
        b->size = size;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = std::max<VkDeviceSize>(size, 4);
        bi.usage = usage;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (c->vkCreateBuffer(c->device, &bi, nullptr, &b->buffer) != VK_SUCCESS) return nullptr;
        VkMemoryRequirements req;
        c->vkGetBufferMemoryRequirements(c->device, b->buffer, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = c->memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || c->vkAllocateMemory(c->device, &ai, nullptr, &b->memory) != VK_SUCCESS ||
            c->vkBindBufferMemory(c->device, b->buffer, b->memory, 0) != VK_SUCCESS ||
            c->vkMapMemory(c->device, b->memory, 0, VK_WHOLE_SIZE, 0, &b->mapped) != VK_SUCCESS)
            return nullptr;
        return b;
    }
    ~host_buffer()
    {
        if (mapped) c->vkUnmapMemory(c->device, memory);
        if (buffer) c->vkDestroyBuffer(c->device, buffer, nullptr);
        if (memory) c->vkFreeMemory(c->device, memory, nullptr);
    }
};

class vk_device;

class vk_texture : public texture
{
public:
    int width() const override { return w; }
    int height() const override { return h; }
    gpu::format format() const override { return f; }
    ~vk_texture() override
    {
        for (VkImageView v : level_views)
            if (v) c->vkDestroyImageView(c->device, v, nullptr);
        if (view) c->vkDestroyImageView(c->device, view, nullptr);
        if (image) c->vkDestroyImage(c->device, image, nullptr);
        if (memory) c->vkFreeMemory(c->device, memory, nullptr);
    }

    const vk_device* owner = nullptr;
    core_ptr c;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;      // every level, for sampling
    std::vector<VkImageView> level_views;  // one level each, for drawing into (made when first needed)
    int w = 0, h = 0, levels = 1;
    gpu::format f = format::rgba8;
    bool render_target = false;
};

class vk_program : public program
{
public:
    ~vk_program() override
    {
        for (auto& [format, pipeline] : pipelines) c->vkDestroyPipeline(c->device, pipeline, nullptr);
        if (module) c->vkDestroyShaderModule(c->device, module, nullptr);
    }
    const vk_device* owner = nullptr;
    core_ptr c;
    VkShaderModule module = VK_NULL_HANDLE;
    std::string entry;
    mutable std::map<VkFormat, VkPipeline> pipelines;  // by target format; under the device's lock
};

// Done once set: a readback's, finished by the device's thread.
class flag_line : public timeline
{
public:
    uint64_t completed() const override
    {
        std::lock_guard<std::mutex> l(m_);
        return set_ ? 1 : 0;
    }
    void wait(uint64_t) const override
    {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return set_; });
    }
    void set()
    {
        {
            std::lock_guard<std::mutex> l(m_);
            set_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex m_;
    mutable std::condition_variable cv_;
    bool set_ = false;
};

// A level of an image from one layout to another, with everything before
// finished and everything after waiting (simple, and always right).
void barrier(const core& c, VkCommandBuffer cmd, VkImage image, uint32_t level, uint32_t levels, VkImageLayout from,
             VkImageLayout to)
{
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, levels, 0, 1};
    c.vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &b);
}

class vk_device : public device, public std::enable_shared_from_this<vk_device>
{
public:
    // A ticket outliving its device is done: nothing it waited for is left.
    class line : public timeline
    {
    public:
        explicit line(std::weak_ptr<vk_device> d) : d_(std::move(d)) {}
        uint64_t completed() const override
        {
            auto d = d_.lock();
            return d ? d->completed() : UINT64_MAX;
        }
        void wait(uint64_t v) const override
        {
            if (auto d = d_.lock()) d->wait_for(v);
        }

    private:
        std::weak_ptr<vk_device> d_;
    };

    static std::shared_ptr<vk_device> make(uint64_t want)
    {
        auto c = std::make_shared<core>();
        if (!open(*c, want)) return nullptr;
        // As on Direct3D: the last reference going on the worker deletes
        // from another thread, which joins the worker.
        auto d = std::shared_ptr<vk_device>(new vk_device,
                                            [](vk_device* p)
                                            {
                                                if (p->worker_.get_id() == std::this_thread::get_id())
                                                    std::thread([p] { delete p; }).detach();
                                                else
                                                    delete p;
                                            });
        d->c_ = c;
        if (!d->init()) return nullptr;
        d->line_ = std::make_shared<line>(d);
        d->worker_ = std::thread([raw = d.get()] { raw->work(); });
        return d;
    }

    ~vk_device() override
    {
        {
            std::lock_guard<std::mutex> l(work_m_);
            stop_ = true;
        }
        work_cv_.notify_all();
        if (worker_.joinable()) worker_.join();
        core& c = *c_;
        c.vkDeviceWaitIdle(c.device);
        for (VkSampler s : samplers_)
            if (s) c.vkDestroySampler(c.device, s, nullptr);
        if (layout_) c.vkDestroyPipelineLayout(c.device, layout_, nullptr);
        if (set_layout_) c.vkDestroyDescriptorSetLayout(c.device, set_layout_, nullptr);
        if (vs_) c.vkDestroyShaderModule(c.device, vs_, nullptr);
        if (timeline_) c.vkDestroySemaphore(c.device, timeline_, nullptr);
        if (pool_) c.vkDestroyCommandPool(c.device, pool_, nullptr);
    }

    std::string_view api() const override { return "vulkan"; }
    bool software() const override { return c_->properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU; }
    bool takes(shader_language l) const override { return l == shader_language::spirv; }
    memory_budget memory() const override
    {
        const core& c = *c_;
        memory_budget m;
        if (c.budget_ext)
        {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT b{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
            VkPhysicalDeviceMemoryProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b};
            c.vkGetPhysicalDeviceMemoryProperties2(c.physical, &p);
            for (uint32_t i = 0; i < p.memoryProperties.memoryHeapCount; ++i)
                if (p.memoryProperties.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
                    m.budget += b.heapBudget[i], m.used += b.heapUsage[i];
            return m;
        }
        for (uint32_t i = 0; i < c.memory.memoryHeapCount; ++i)
            if (c.memory.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) m.budget += c.memory.memoryHeaps[i].size;
        return m;
    }
    int max_texture_size() const override { return int(c_->properties.limits.maxImageDimension2D); }
    bool lost() const override { return lost_; }

    texture_ptr create(const texture_desc& desc) override
    {
        core& c = *c_;
        const int levels = std::max(1, desc.levels);
        if (desc.width <= 0 || desc.height <= 0 || desc.width > max_texture_size() ||
            desc.height > max_texture_size() || (std::max(desc.width, desc.height) >> (levels - 1)) < 1)
            return nullptr;
        const VkFormat vf = vk_format(desc.format);
        VkFormatProperties fp;
        c.vkGetPhysicalDeviceFormatProperties(c.physical, vf, &fp);
        const VkFormatFeatureFlags need =
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT |
            (desc.render_target ? VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT : 0);
        if ((fp.optimalTilingFeatures & need) != need) return nullptr;

        auto t = std::make_shared<vk_texture>();
        t->owner = this;
        t->c = c_;
        t->w = desc.width, t->h = desc.height, t->levels = levels, t->f = desc.format;
        t->render_target = desc.render_target;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = vf;
        ii.extent = {uint32_t(desc.width), uint32_t(desc.height), 1};
        ii.mipLevels = uint32_t(levels);
        ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   (desc.render_target ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
        ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (c.vkCreateImage(c.device, &ii, nullptr, &t->image) != VK_SUCCESS) return nullptr;
        VkMemoryRequirements req;
        c.vkGetImageMemoryRequirements(c.device, t->image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = c.memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX || check(c.vkAllocateMemory(c.device, &ai, nullptr, &t->memory)) ||
            c.vkBindImageMemory(c.device, t->image, t->memory, 0) != VK_SUCCESS)
            return nullptr;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = t->image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = vf;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, uint32_t(levels), 0, 1};
        if (c.vkCreateImageView(c.device, &vi, nullptr, &t->view) != VK_SUCCESS) return nullptr;
        t->level_views.resize(size_t(levels), VK_NULL_HANDLE);
        // Into the layout it rests in, cleared to nothing.
        submit([&](VkCommandBuffer cmd)
               {
                   barrier(c, cmd, t->image, 0, uint32_t(levels), VK_IMAGE_LAYOUT_UNDEFINED,
                           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
               },
               {});
        return t;
    }

    ticket upload(const texture_ptr& tp, std::span<const std::byte> pixels, const source_layout& layout, int level,
                  rect region) override
    {
        auto* t = mine(tp);
        if (!t || level < 0 || level >= t->levels) return {};
        const int lw = std::max(1, t->w >> level), lh = std::max(1, t->h >> level);
        if (region.empty()) region = {0, 0, lw, lh};
        region.w = std::min(region.w, lw - region.x), region.h = std::min(region.h, lh - region.y);
        if (region.x < 0 || region.y < 0 || region.empty()) return {};
        const size_t stride = bytes_per_pixel(t->f) * size_t(region.w);
        auto staging = host_buffer::make(c_, stride * size_t(region.h), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        if (!staging) return {};
        convert(pixels, layout, region.w, region.h, t->f, static_cast<std::byte*>(staging->mapped), stride);
        const core& c = *c_;
        const uint64_t value = submit(
            [&](VkCommandBuffer cmd)
            {
                barrier(c, cmd, t->image, uint32_t(level), 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(level), 0, 1};
                copy.imageOffset = {region.x, region.y, 0};
                copy.imageExtent = {uint32_t(region.w), uint32_t(region.h), 1};
                c.vkCmdCopyBufferToImage(cmd, staging->buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                barrier(c, cmd, t->image, uint32_t(level), 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            },
            {staging, tp});
        return ticket(line_, value);
    }

    ticket readback(const texture_ptr& tp, std::span<std::byte> out, size_t stride, int level,
                    std::function<void()> done) override
    {
        auto* t = mine(tp);
        if (!t || level < 0 || level >= t->levels) return {};
        const int lw = std::max(1, t->w >> level), lh = std::max(1, t->h >> level);
        const size_t row = bytes_per_pixel(t->f) * size_t(lw);
        if (stride < row || out.size() < stride * size_t(lh - 1) + row) return {};
        auto staging = host_buffer::make(c_, row * size_t(lh), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        if (!staging) return {};
        const core& c = *c_;
        const uint64_t value = submit(
            [&](VkCommandBuffer cmd)
            {
                barrier(c, cmd, t->image, uint32_t(level), 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                VkBufferImageCopy copy{};
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(level), 0, 1};
                copy.imageExtent = {uint32_t(lw), uint32_t(lh), 1};
                c.vkCmdCopyImageToBuffer(cmd, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging->buffer, 1, &copy);
                barrier(c, cmd, t->image, uint32_t(level), 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            },
            {tp});
        auto flag = std::make_shared<flag_line>();
        queue(value, [staging, out, stride, row, lh, flag, done = std::move(done)]
              {
                  for (int y = 0; y < lh; ++y)
                      std::memcpy(out.data() + size_t(y) * stride,
                                  static_cast<const std::byte*>(staging->mapped) + size_t(y) * row, row);
                  flag->set();
                  if (done) done();
              });
        return ticket(flag, 1);
    }

    program_result program(const shader& pixel) override
    {
        for (const shader::form& f : pixel.forms)
        {
            if (f.language != shader_language::spirv) continue;
            if (f.code.size() % 4 || f.code.empty())
                return {nullptr, (pixel.name.empty() ? "shader" : pixel.name) + ": SPIR-V isn't whole words"};
            auto p = std::make_shared<vk_program>();
            p->owner = this;
            p->c = c_;
            p->entry = pixel.entry;
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = f.code.size();
            mi.pCode = reinterpret_cast<const uint32_t*>(f.code.data());
            if (c_->vkCreateShaderModule(c_->device, &mi, nullptr, &p->module) == VK_SUCCESS) return {p, {}};
            return {nullptr, (pixel.name.empty() ? "shader" : pixel.name) + ": the SPIR-V wasn't taken"};
        }
        return {nullptr, (pixel.name.empty() ? "shader" : pixel.name) + ": no form Vulkan takes (spirv)"};
    }

    ticket run(std::span<const pass> passes) override
    {
        const core& c = *c_;
        // Every pass's uniform block, each at an aligned offset of one buffer.
        const VkDeviceSize align = std::max<VkDeviceSize>(16, c.properties.limits.minUniformBufferOffsetAlignment);
        std::vector<VkDeviceSize> offsets;
        VkDeviceSize total = 0;
        for (const pass& p : passes)
        {
            offsets.push_back(total);
            total += (VkDeviceSize(std::max<size_t>(p.uniforms.size(), 16)) + align - 1) / align * align;
        }
        auto uniforms = host_buffer::make(c_, total, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
        if (!uniforms) return {};
        std::memset(uniforms->mapped, 0, size_t(total));
        for (size_t i = 0; i < passes.size(); ++i)
            std::memcpy(static_cast<std::byte*>(uniforms->mapped) + offsets[i], passes[i].uniforms.data(),
                        passes[i].uniforms.size());

        std::vector<std::shared_ptr<const void>> held{uniforms};
        const uint64_t value = submit(
            [&](VkCommandBuffer cmd)
            {
                for (size_t i = 0; i < passes.size(); ++i)
                {
                    const pass& p = passes[i];
                    auto* prog = dynamic_cast<const vk_program*>(p.program.get());
                    auto* target = mine(p.target);
                    if (!prog || prog->owner != this || !target || !target->render_target || p.target_level < 0 ||
                        p.target_level >= target->levels)
                        continue;
                    const VkPipeline pipeline = pipeline_for(*prog, vk_format(target->f));
                    const VkImageView view = level_view(*target, p.target_level);
                    if (!pipeline || !view) continue;
                    const uint32_t level = uint32_t(p.target_level);
                    const int lw = std::max(1, target->w >> level), lh = std::max(1, target->h >> level);
                    const rect r = p.region.empty() ? rect{0, 0, lw, lh} : p.region;

                    barrier(c, cmd, target->image, level, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                    VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                    att.imageView = view;
                    att.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;  // a region keeps the rest
                    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
                    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                    ri.renderArea = {{0, 0}, {uint32_t(lw), uint32_t(lh)}};
                    ri.layerCount = 1;
                    ri.colorAttachmentCount = 1;
                    ri.pColorAttachments = &att;
                    c.vkCmdBeginRendering(cmd, &ri);
                    c.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                    const VkViewport vp{0, 0, float(lw), float(lh), 0, 1};
                    c.vkCmdSetViewport(cmd, 0, 1, &vp);
                    const VkRect2D sc{{r.x, r.y}, {uint32_t(r.w), uint32_t(r.h)}};
                    c.vkCmdSetScissor(cmd, 0, 1, &sc);

                    VkDescriptorImageInfo images[max_inputs]{}, samplers[max_inputs]{};
                    std::vector<VkWriteDescriptorSet> writes;
                    const size_t n = std::min<size_t>(max_inputs, p.inputs.size());
                    for (size_t k = 0; k < n; ++k)
                    {
                        auto* in = mine(p.inputs[k]);
                        if (!in) continue;
                        images[k] = {VK_NULL_HANDLE, in->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                        const sampler s = k < p.samplers.size() ? p.samplers[k] : sampler::nearest_clamp;
                        samplers[k] = {samplers_[int(s)], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                        w.dstBinding = uint32_t(k);
                        w.descriptorCount = 1;
                        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                        w.pImageInfo = &images[k];
                        writes.push_back(w);
                        w.dstBinding = uint32_t(max_inputs + k);
                        w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                        w.pImageInfo = &samplers[k];
                        writes.push_back(w);
                        held.push_back(p.inputs[k]);
                    }
                    const VkDescriptorBufferInfo ub{uniforms->buffer, offsets[i],
                                                    VkDeviceSize(std::max<size_t>(p.uniforms.size(), 16))};
                    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    w.dstBinding = 2 * max_inputs;
                    w.descriptorCount = 1;
                    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
                    w.pBufferInfo = &ub;
                    writes.push_back(w);
                    c.vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, uint32_t(writes.size()),
                                                writes.data());
                    c.vkCmdDraw(cmd, 3, 1, 0, 0);
                    c.vkCmdEndRendering(cmd);
                    barrier(c, cmd, target->image, level, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                    held.push_back(p.program);
                    held.push_back(p.target);
                }
            },
            {});
        queue(value, [held = std::move(held)] {});  // kept until the GPU's past them
        return ticket(line_, value);
    }

private:
    vk_device() = default;

    // The loader, an instance, and the device: `want` (a DRM render node's
    // dev_t) if it's there, else the first discrete, integrated, virtual,
    // then CPU one that has Vulkan 1.3 and push descriptors.
    static bool open(core& c, uint64_t want)
    {
        c.library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!c.library) return false;
        auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(c.library, "vkGetInstanceProcAddr"));
        if (!get) return false;
        auto create_instance = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
        auto version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(get(nullptr, "vkEnumerateInstanceVersion"));
        uint32_t v = 0;
        if (!create_instance || !version || version(&v) != VK_SUCCESS || v < VK_API_VERSION_1_3) return false;
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pEngineName = "rapport";
        app.apiVersion = VK_API_VERSION_1_3;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        if (create_instance(&ici, nullptr, &c.instance) != VK_SUCCESS) return false;
#define RAP_VK_LOAD(name)                                                                     \
    if (!(c.name = reinterpret_cast<PFN_##name>(get(c.instance, #name)))) return false;
        RAP_VK_INSTANCE_FNS(RAP_VK_LOAD)
#undef RAP_VK_LOAD

        uint32_t n = 0;
        c.vkEnumeratePhysicalDevices(c.instance, &n, nullptr);
        std::vector<VkPhysicalDevice> all(n);
        c.vkEnumeratePhysicalDevices(c.instance, &n, all.data());
        const bool software_only = std::getenv("RAPPORT_GPU") && std::string(std::getenv("RAPPORT_GPU")) == "software";
        int best = -1, best_rank = 99;
        for (uint32_t i = 0; i < n; ++i)
        {
            VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
            VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            const bool has_drm = has_extension(c, all[i], VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME);
            if (has_drm) p.pNext = &drm;
            c.vkGetPhysicalDeviceProperties2(all[i], &p);
            if (p.properties.apiVersion < VK_API_VERSION_1_3 ||
                !has_extension(c, all[i], VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) || graphics_family(c, all[i]) < 0)
                continue;
            int rank = 0;
            switch (p.properties.deviceType)
            {
                case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: rank = 1; break;
                case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: rank = 2; break;
                case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: rank = 3; break;
                case VK_PHYSICAL_DEVICE_TYPE_CPU: rank = 4; break;
                default: rank = 5; break;
            }
            if (software_only && p.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
            if (want && has_drm && drm.hasRender && makedev(uint32_t(drm.renderMajor), uint32_t(drm.renderMinor)) == want)
                rank = 0;
            if (rank < best_rank) best = int(i), best_rank = rank;
        }
        if (best < 0) return false;
        c.physical = all[size_t(best)];
        c.vkGetPhysicalDeviceMemoryProperties(c.physical, &c.memory);
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        c.vkGetPhysicalDeviceProperties2(c.physical, &props);
        c.properties = props.properties;
        c.budget_ext = has_extension(c, c.physical, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

        const float priority = 1;
        VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qi.queueFamilyIndex = uint32_t(graphics_family(c, c.physical));
        qi.queueCount = 1;
        qi.pQueuePriorities = &priority;
        std::vector<const char*> extensions{VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
        if (c.budget_ext) extensions.push_back(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        f13.dynamicRendering = VK_TRUE;
        f13.synchronization2 = VK_FALSE;
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        f12.timelineSemaphore = VK_TRUE;
        VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12};
        di.queueCreateInfoCount = 1;
        di.pQueueCreateInfos = &qi;
        di.enabledExtensionCount = uint32_t(extensions.size());
        di.ppEnabledExtensionNames = extensions.data();
        if (c.vkCreateDevice(c.physical, &di, nullptr, &c.device) != VK_SUCCESS) return false;
#define RAP_VK_LOAD(name)                                                                       \
    if (!(c.name = reinterpret_cast<PFN_##name>(c.vkGetDeviceProcAddr(c.device, #name)))) return false;
        RAP_VK_DEVICE_FNS(RAP_VK_LOAD)
#undef RAP_VK_LOAD
        return true;
    }
    static bool has_extension(const core& c, VkPhysicalDevice d, const char* name)
    {
        uint32_t n = 0;
        c.vkEnumerateDeviceExtensionProperties(d, nullptr, &n, nullptr);
        std::vector<VkExtensionProperties> all(n);
        c.vkEnumerateDeviceExtensionProperties(d, nullptr, &n, all.data());
        return std::any_of(all.begin(), all.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
    }
    static int graphics_family(const core& c, VkPhysicalDevice d)
    {
        uint32_t n = 0;
        c.vkGetPhysicalDeviceQueueFamilyProperties(d, &n, nullptr);
        std::vector<VkQueueFamilyProperties> all(n);
        c.vkGetPhysicalDeviceQueueFamilyProperties(d, &n, all.data());
        for (uint32_t i = 0; i < n; ++i)
            if (all[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) return int(i);
        return -1;
    }

    // The queue, the timeline, the fixed state.
    bool init()
    {
        core& c = *c_;
        family_ = uint32_t(graphics_family(c, c.physical));
        c.vkGetDeviceQueue(c.device, family_, 0, &queue_);
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = family_;
        VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti};
        if (c.vkCreateCommandPool(c.device, &pi, nullptr, &pool_) != VK_SUCCESS ||
            c.vkCreateSemaphore(c.device, &si, nullptr, &timeline_) != VK_SUCCESS)
            return false;

        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof k_fullscreen_vs;
        mi.pCode = k_fullscreen_vs;
        if (c.vkCreateShaderModule(c.device, &mi, nullptr, &vs_) != VK_SUCCESS) return false;

        VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        s.addressModeU = s.addressModeV = s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.magFilter = s.minFilter = VK_FILTER_NEAREST;
        s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.maxLod = 0;
        if (c.vkCreateSampler(c.device, &s, nullptr, &samplers_[0]) != VK_SUCCESS) return false;
        s.magFilter = s.minFilter = VK_FILTER_LINEAR;
        if (c.vkCreateSampler(c.device, &s, nullptr, &samplers_[1]) != VK_SUCCESS) return false;
        s.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        s.maxLod = VK_LOD_CLAMP_NONE;
        if (c.vkCreateSampler(c.device, &s, nullptr, &samplers_[2]) != VK_SUCCESS) return false;

        // Set 0: textures 0..7, samplers 8..15, the uniform block 16.
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        for (uint32_t k = 0; k < uint32_t(max_inputs); ++k)
        {
            bindings.push_back({k, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
            bindings.push_back({uint32_t(max_inputs) + k, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        }
        bindings.push_back({uint32_t(2 * max_inputs), VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = uint32_t(bindings.size());
        li.pBindings = bindings.data();
        if (c.vkCreateDescriptorSetLayout(c.device, &li, nullptr, &set_layout_) != VK_SUCCESS) return false;
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &set_layout_;
        return c.vkCreatePipelineLayout(c.device, &pli, nullptr, &layout_) == VK_SUCCESS;
    }

    vk_texture* mine(const texture_ptr& t) const
    {
        auto* d = dynamic_cast<vk_texture*>(t.get());
        return d && d->owner == this ? d : nullptr;
    }

    bool check(VkResult r)
    {
        if (r == VK_ERROR_DEVICE_LOST) lost_ = true;
        return r != VK_SUCCESS;
    }

    // A pass's pipeline for a target format; under the lock.
    VkPipeline pipeline_for(const vk_program& p, VkFormat f)
    {
        if (auto it = p.pipelines.find(f); it != p.pipelines.end()) return it->second;
        const core& c = *c_;
        VkPipelineShaderStageCreateInfo stages[2]{{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                  {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs_;
        stages[0].pName = "main";
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = p.module;
        stages[1].pName = p.entry.c_str();
        VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vps.viewportCount = vps.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                               VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 1;
        cb.pAttachments = &blend;
        const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkPipelineRenderingCreateInfo rinfo{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rinfo.colorAttachmentCount = 1;
        rinfo.pColorAttachmentFormats = &f;
        VkGraphicsPipelineCreateInfo gi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rinfo};
        gi.stageCount = 2;
        gi.pStages = stages;
        gi.pVertexInputState = &vin;
        gi.pInputAssemblyState = &ia;
        gi.pViewportState = &vps;
        gi.pRasterizationState = &rs;
        gi.pMultisampleState = &ms;
        gi.pColorBlendState = &cb;
        gi.pDynamicState = &ds;
        gi.layout = layout_;
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (c.vkCreateGraphicsPipelines(c.device, VK_NULL_HANDLE, 1, &gi, nullptr, &pipeline) != VK_SUCCESS)
            return VK_NULL_HANDLE;
        p.pipelines[f] = pipeline;
        return pipeline;
    }

    // A view of one level, to draw into; under the lock.
    VkImageView level_view(vk_texture& t, int level)
    {
        VkImageView& v = t.level_views[size_t(level)];
        if (!v)
        {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = t.image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = vk_format(t.f);
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, uint32_t(level), 1, 0, 1};
            if (c_->vkCreateImageView(c_->device, &vi, nullptr, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
        }
        return v;
    }

    // Records `fill` into a command buffer and submits it, signalling the
    // next point on the timeline; `held` (and the command buffer) live until
    // the GPU's past it.
    uint64_t submit(const std::function<void(VkCommandBuffer)>& fill, std::vector<std::shared_ptr<const void>> held)
    {
        const core& c = *c_;
        std::lock_guard<std::mutex> l(m_);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = pool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        if (check(c.vkAllocateCommandBuffers(c.device, &ai, &cmd))) return next_;
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        c.vkBeginCommandBuffer(cmd, &bi);
        fill(cmd);
        c.vkEndCommandBuffer(cmd);
        const uint64_t value = ++next_;
        VkTimelineSemaphoreSubmitInfo ts{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
        ts.signalSemaphoreValueCount = 1;
        ts.pSignalSemaphoreValues = &value;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &timeline_;
        check(c.vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE));
        queue(value, [this, cmd, held = std::move(held)]
              {
                  std::lock_guard<std::mutex> l(m_);
                  c_->vkFreeCommandBuffers(c_->device, pool_, 1, &cmd);
              });
        return value;
    }

    uint64_t completed() const
    {
        uint64_t v = 0;
        if (c_->vkGetSemaphoreCounterValue(c_->device, timeline_, &v) == VK_ERROR_DEVICE_LOST) return UINT64_MAX;
        return v;
    }
    void wait_for(uint64_t v) const
    {
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &timeline_;
        wi.pValues = &v;
        c_->vkWaitSemaphores(c_->device, &wi, UINT64_MAX);
    }

    // Work for the device's thread once the GPU reaches `value`, in order.
    void queue(uint64_t value, std::function<void()> f)
    {
        {
            std::lock_guard<std::mutex> l(work_m_);
            work_.emplace_back(value, std::move(f));
        }
        work_cv_.notify_one();
    }
    void work()
    {
        for (;;)
        {
            std::pair<uint64_t, std::function<void()>> item;
            {
                std::unique_lock<std::mutex> l(work_m_);
                work_cv_.wait(l, [&] { return stop_ || !work_.empty(); });
                if (work_.empty()) return;  // stopping, and nothing left
                item = std::move(work_.front());
                work_.pop_front();
            }
            wait_for(item.first);
            item.second();
        }
    }

    core_ptr c_;
    uint32_t family_ = 0;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkSemaphore timeline_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE;
    VkSampler samplers_[3] = {};
    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    std::shared_ptr<line> line_;
    std::mutex m_;  // the queue, the pool, pipelines and views
    uint64_t next_ = 0;
    bool lost_ = false;

    std::mutex work_m_;
    std::condition_variable work_cv_;
    std::deque<std::pair<uint64_t, std::function<void()>>> work_;
    bool stop_ = false;
    std::thread worker_;
};

}  // namespace

std::shared_ptr<device> make_vulkan_headless(uint64_t adapter)
{
    return vk_device::make(adapter);
}

}  // namespace rap::gpu
