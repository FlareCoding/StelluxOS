#ifndef STLXDM_GPU_COMPOSITOR_HPP
#define STLXDM_GPU_COMPOSITOR_HPP

#include "compare.hpp"
#include "damage.hpp"
#include "frame_stats.hpp"
#include "layers.hpp"
#include "picture.hpp"
#include "presenter.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

struct gpu_info;

/* How long stlxdm waits for a compose to end before it stops composing on
 * the GPU */
constexpr uint64_t COMPOSE_DEADLINE_NS = 100000000;

/* Memory a compose writes: its first pixel's GPU address, the bytes from
 * one row to the next, and the output buffer it lies in when the display
 * scans it out, made by the same compositor, null for a picture such as
 * the collapse backdrop */
struct compose_target {
    uint64_t             address = 0;
    uint32_t             pitch = 0;
    const output_buffer* scanout = nullptr;

    /* With `scanout`, a picture of the same compositor as large as
     * `scanout`, which takes a copy of each row the compose wrote at the
     * same offset so the CPU can compare the frame */
    const picture*       readback = nullptr;
};

/* How a compose ended */
enum class compose_result {
    composed,       /* its target holds the frame */
    not_submitted,  /* memory ran out before the GPU had any of it, and the twin may lack rows the CPU changed */
    lost,           /* the device is lost, so none of the compositor's work runs any more */
    unfinished,     /* it did not end within `COMPOSE_DEADLINE_NS`, or its wait failed, and may still run */
};

/* Where the last compose stands, read without waiting */
enum class compose_state {
    ended,    /* its target holds the frame, or no compose was submitted */
    running,  /* it may still read and write the compositor's memory */
    lost,     /* the device is lost, so none of the compositor's work runs any more */
};

/* A failure a test has the next compose make: a read of an address no
 * context maps, which faults, or an endless spin, which runs past
 * `COMPOSE_DEADLINE_NS`. Either loses the compositor's context once the
 * kernel ends it, at the fault or at the GPU's hang deadline. */
enum class compose_failure {
    unmapped_read,
    spin,
};

/* How a window buffer's import went */
enum class window_import_result {
    imported,
    no_memory,  /* Vulkan, or the handle table for the import's copy of the handle, had no memory left */
    refused,    /* a buffer the compositor cannot read */
};

/* The GPU compositor composes an output's frame on the GPU, in one compute
 * pass that reads each window's picture where it lies, through the Vulkan
 * library on the GPU of stlxdm's /dev/gpu0 file.
 *
 * It owns a Vulkan device with one queue, the compose pipeline and
 * stlxdm's timeline, which each compose's submission raises and stlxdm
 * waits on before presenting. It also holds a probe context on the
 * /dev/gpu0 file, with no queue and no mapping, which reads lost with
 * `GPU_LOST_DEVICE` once the GPU failed and tells that apart from a loss
 * of the compositor's own context.
 *
 * stlxdm's one thread makes every call. */
class gpu_compositor : public twin_maker, public output_buffer_maker {
public:
    class compose_buffers;

    /* Creates the compositor on the GPU of `gpu_fd`, which must outlive it,
     * or returns null after printing why stlxdm composes on the CPU */
    static std::unique_ptr<gpu_compositor> create(int gpu_fd);

    ~gpu_compositor() override;

    gpu_compositor(const gpu_compositor&) = delete;
    gpu_compositor& operator=(const gpu_compositor&) = delete;

    std::unique_ptr<picture> make_picture(size_t bytes) override;
    std::unique_ptr<picture> make_twin(size_t bytes) override;

    /* The bytes from one row to the next of every output buffer `width`
     * pixels wide this compositor makes */
    static uint32_t output_pitch(uint32_t width);

    /* A scanout buffer in video memory, which gfxfb imports through a copy
     * of its handle that can only read it and Vulkan through the handle
     * that created it */
    std::unique_ptr<output_buffer> make_output_buffer(
        int gfxfb_fd,
        uint32_t output,
        uint32_t width,
        uint32_t height
    ) override;

    /* Imports the buffer the /dev/gpu0 handle `gpu_buffer` names, `bytes`
     * long in memory of `placement`, mappable when `is_mappable`, through a
     * copy of the handle narrowed to reading. `gpu_buffer` must be able to
     * read the buffer and stays the caller's. `imported` stays empty on
     * failure. */
    window_import_result import_window_buffer(
        int gpu_buffer,
        uint64_t bytes,
        uint32_t placement,
        bool is_mappable,
        std::unique_ptr<window_import>& imported
    );

    /* Whether the probe context reads lost with `GPU_LOST_DEVICE` */
    bool has_gpu_failed() const;

    /* An output's compose buffers, whose layer buffer its first compose
     * makes, or null when Vulkan has no memory left */
    std::unique_ptr<compose_buffers> make_compose_buffers();

    /* Composes `rects` of `scene`, in the output's pixels and clipped to
     * it, into `target` through `buffers`, after copying into their twins
     * the rows the CPU changed in `backdrop` and in `frost` (null without
     * one), and waits at most `COMPOSE_DEADLINE_NS` for the compose to end.
     * Every picture and import the scene, `backdrop` and `frost` name must
     * be this compositor's and stay until the compose ended. The rects must
     * not overlap, since their dispatches may run at the same time. */
    compose_result compose(
        compose_buffers& buffers,
        const compose_target& target,
        const layer_scene& scene,
        const std::vector<damage_list::rect>& rects,
        backdrop_picture& backdrop,
        frost_picture* frost
    );

    /* Has the next compose that lists a rectangle make `failure` at one
     * pixel of its last rectangle, after its layers. False for an unmapped
     * read on a GPU whose contexts may map address 0, the address
     * it reads. */
    bool fail_next_compose(compose_failure failure);

    /* Keeps the memory and command buffers of everything dropped from here
     * on until `end_retirement` or the destructor, since a compose past
     * `COMPOSE_DEADLINE_NS` may still use them */
    void retire();

    /* Frees the memory kept since `retire`, after which `destroy_buffer`
     * frees at once again. The caller must call it only when no compose
     * runs and none will follow. The command buffers kept stay with
     * the pool. */
    void end_retirement();

    bool is_retired() const;
    compose_state last_compose_state() const;

    /* What the last compose that composed cost */
    const compose_times& last_times() const { return m_last_times; }

private:
    class owned_picture;
    class scanout_buffer;
    class imported_window_buffer;

    /* The properties a memory type must have, and those it must lack */
    struct memory_kind {
        VkMemoryPropertyFlags required;
        VkMemoryPropertyFlags denied;
    };

    static const memory_kind COHERENT_MEMORY;
    static const memory_kind PICTURE_MEMORY;
    static const memory_kind VIDEO_MEMORY;
    static const memory_kind SYSTEM_WINDOW_MEMORY;
    static const memory_kind VIDEO_WINDOW_MEMORY;

    /* The library exports only the global commands, so the others come
     * from `vkGetInstanceProcAddr` */
    struct commands {
        PFN_vkDestroyInstance                           destroy_instance;
        PFN_vkEnumeratePhysicalDevices                  enumerate_physical_devices;
        PFN_vkEnumerateDeviceExtensionProperties        enumerate_device_extensions;
        PFN_vkGetPhysicalDeviceProperties2              get_physical_device_properties;
        PFN_vkGetPhysicalDeviceFeatures2                get_physical_device_features;
        PFN_vkGetPhysicalDeviceMemoryProperties         get_memory_properties;
        PFN_vkGetPhysicalDeviceQueueFamilyProperties    get_queue_families;
        PFN_vkGetPhysicalDeviceExternalBufferProperties get_external_buffer_properties;
        PFN_vkCreateDevice                              create_device;
        PFN_vkDestroyDevice                             destroy_device;
        PFN_vkGetDeviceQueue                            get_device_queue;
        PFN_vkCreateSemaphore                           create_semaphore;
        PFN_vkDestroySemaphore                          destroy_semaphore;
        PFN_vkCreateBuffer                              create_buffer;
        PFN_vkDestroyBuffer                             destroy_buffer;
        PFN_vkGetBufferMemoryRequirements               get_buffer_memory_requirements;
        PFN_vkAllocateMemory                            allocate_memory;
        PFN_vkFreeMemory                                free_memory;
        PFN_vkBindBufferMemory                          bind_buffer_memory;
        PFN_vkMapMemory                                 map_memory;
        PFN_vkGetBufferDeviceAddress                    get_buffer_device_address;
        PFN_vkCreateShaderModule                        create_shader_module;
        PFN_vkDestroyShaderModule                       destroy_shader_module;
        PFN_vkCreatePipelineLayout                      create_pipeline_layout;
        PFN_vkDestroyPipelineLayout                     destroy_pipeline_layout;
        PFN_vkCreateComputePipelines                    create_compute_pipelines;
        PFN_vkDestroyPipeline                           destroy_pipeline;
        PFN_vkCreateCommandPool                         create_command_pool;
        PFN_vkDestroyCommandPool                        destroy_command_pool;
        PFN_vkAllocateCommandBuffers                    allocate_command_buffers;
        PFN_vkFreeCommandBuffers                        free_command_buffers;
        PFN_vkBeginCommandBuffer                        begin_command_buffer;
        PFN_vkEndCommandBuffer                          end_command_buffer;
        PFN_vkCmdBindPipeline                           cmd_bind_pipeline;
        PFN_vkCmdPushConstants                          cmd_push_constants;
        PFN_vkCmdDispatch                               cmd_dispatch;
        PFN_vkCmdPipelineBarrier                        cmd_pipeline_barrier;
        PFN_vkCmdCopyBuffer                             cmd_copy_buffer;
        PFN_vkCreateQueryPool                           create_query_pool;
        PFN_vkDestroyQueryPool                          destroy_query_pool;
        PFN_vkGetQueryPoolResults                       get_query_pool_results;
        PFN_vkCmdResetQueryPool                         cmd_reset_query_pool;
        PFN_vkCmdWriteTimestamp                         cmd_write_timestamp;
        PFN_vkQueueSubmit                               queue_submit;
        PFN_vkWaitSemaphores                            wait_semaphores;
        PFN_vkGetSemaphoreCounterValue                  get_semaphore_counter_value;
    };

    /* A buffer bound to memory of its own, with its GPU address and, when
     * `create_buffer` made it in host-visible memory, its CPU mapping */
    struct device_buffer {
        VkBuffer        buffer = VK_NULL_HANDLE;
        VkDeviceMemory  memory = VK_NULL_HANDLE;
        VkDeviceAddress address = 0;
        void*           mapping = nullptr;
    };

    gpu_compositor() = default;

    const char* start(int gpu_fd);
    bool find_commands();
    const char* create_instance();
    bool offers_extension(VkPhysicalDevice physical_device, const char* name);
    const char* find_physical_device(const gpu_info& info);
    const char* check_physical_device();
    const char* create_device();
    const char* create_pipeline();
    const char* create_command_pool();
    const char* create_timestamp_queries();
    const char* upload_corner_distances();
    const char* make_material_buffer();
    void upload_material();
    const char* probe_gpu_file(int gpu_fd, const gpu_info& info);

    VkResult create_import_buffer(VkDeviceSize size, VkBuffer& created);
    bool import_into_gfxfb(scanout_buffer& buffer, int gpu_buffer, uint32_t output, uint32_t width, uint32_t height);
    VkResult import_memory(int gpu_buffer, uint64_t size, uint32_t type, VkDeviceMemory& imported);
    VkResult import_window_memory(
        int gpu_buffer,
        uint64_t bytes,
        uint32_t placement,
        bool is_mappable,
        device_buffer& import
    );

    bool list_layers(compose_buffers& buffers, const layer_scene& scene, const std::vector<damage_list::rect>& rects);
    layer failure_layer(const damage_list::rect& r) const;
    bool record_compose(
        compose_buffers& buffers,
        const compose_target& target,
        const layer_scene& scene,
        const backdrop_picture& backdrop,
        const frost_picture* frost
    );
    void record_copies(
        VkCommandBuffer commands,
        const backdrop_picture& picture,
        const std::vector<picture_copy>& copies,
        std::vector<VkBufferCopy>& regions
    );
    void record_readback(compose_buffers& buffers, const compose_target& target);
    void write_timestamp(VkCommandBuffer commands, uint32_t query);
    compose_result submit_and_wait(compose_buffers& buffers, uint64_t& submitted_ns);
    bool read_gpu_time(uint64_t& gpu_ns) const;
    void transfer_buffers(
        compose_buffers& buffers,
        const compose_target& target,
        const layer_scene& scene,
        bool to_compose
    );
    VkBufferMemoryBarrier transfer_barrier(VkBuffer buffer, VkAccessFlags accesses, bool to_compose) const;

    uint32_t find_memory_type(uint32_t type_bits, const memory_kind& kind) const;

    /* Leaves what it made in `created` when it fails,
     * for `destroy_buffer` */
    bool create_buffer(
        VkDeviceSize size,
        VkBufferUsageFlags usage,
        const memory_kind& kind,
        device_buffer& created
    );

    void destroy_buffer(device_buffer& buffer);
    void free_buffer(device_buffer& buffer);

    commands                         m_vk = {};
    VkInstance                       m_instance = VK_NULL_HANDLE;
    VkPhysicalDevice                 m_physical_device = VK_NULL_HANDLE;
    char                             m_device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
    VkPhysicalDeviceMemoryProperties m_memory_properties = {};
    uint32_t                         m_queue_family = 0;

    /* How many low bits of the queue's timestamps count, 0 when it writes
     * none, and the nanoseconds of a tick */
    uint32_t                         m_timestamp_bits = 0;
    float                            m_timestamp_period_ns = 0;

    VkDevice         m_device = VK_NULL_HANDLE;
    VkQueue          m_queue = VK_NULL_HANDLE;
    VkSemaphore      m_timeline = VK_NULL_HANDLE;
    VkPipelineLayout m_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline       m_pipeline = VK_NULL_HANDLE;
    VkCommandPool    m_command_pool = VK_NULL_HANDLE;
    VkQueryPool      m_timestamps = VK_NULL_HANDLE;
    int              m_gpu_fd = -1;
    uint32_t         m_page_size = 0;
    bool             m_contexts_may_map_zero = false;
    int              m_probe_context = -1;

    /* The timeline value the last submitted compose raises it to when
     * it ends */
    uint64_t         m_compose_point = 0;

    compose_times    m_last_times;

    /* Whether the compositor is retired, from `retire` to
     * `end_retirement`, and what `destroy_buffer` was handed meanwhile,
     * which `end_retirement` frees */
    bool                       m_is_retired = false;
    std::vector<device_buffer> m_retired_buffers;

    /* decor's corner distance table in GPU memory, its entry count, and the
     * distance past its last entry */
    device_buffer m_corner_distances;
    uint32_t      m_corner_count = 0;
    int32_t       m_corner_far_distance = 0;

    /* The glass tables in GPU memory, as of `material::tables_generation` */
    device_buffer m_material;
    uint32_t      m_material_generation = 0;

    /* The failure the next compose that lists a rectangle makes */
    bool            m_fails_next_compose = false;
    compose_failure m_next_failure = compose_failure::unmapped_read;
};

/* One output's command buffer and the layer buffer its composes' dispatches
 * read, which a compose replaces with a larger one when its lists outgrow
 * it. It must drop before its compositor, and only once no compose recorded
 * in it can run or after `retire`. */
class gpu_compositor::compose_buffers {
public:
    ~compose_buffers();

    compose_buffers(const compose_buffers&) = delete;
    compose_buffers& operator=(const compose_buffers&) = delete;

private:
    friend class gpu_compositor;

    explicit compose_buffers(gpu_compositor& owner) : m_owner(owner) {}

    gpu_compositor&                m_owner;
    VkCommandBuffer                m_commands = VK_NULL_HANDLE;
    device_buffer                  m_layer_buffer;
    VkDeviceSize                   m_layer_buffer_bytes = 0;

    /* A compose's layer lists one after another, and the rectangle and
     * first layer of each */
    std::vector<layer>             m_layers;
    std::vector<damage_list::rect> m_list_rects;
    std::vector<uint32_t>          m_list_starts;

    std::vector<picture_copy>          m_copies;
    std::vector<picture_copy>          m_frost_copies;
    std::vector<VkBufferCopy>          m_copy_regions;
    std::vector<span>                  m_readback_rows;
    std::vector<VkBufferMemoryBarrier> m_transfers;
};

#endif
