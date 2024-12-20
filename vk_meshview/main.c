#include <vulkan/vulkan_core.h>
#define CGLM_FORCE_LEFT_HANDED
#define CGLM_FORCE_DEPTH_ZERO_TO_ONE

#include <cglm/cglm.h>
#include <cglm/struct.h>
#include <cglm/affine-mat.h>
#include <cglm/affine.h>
#include <vulkan/vulkan.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <math.h>

#define WIDTH 1280
#define HEIGHT 720
#define TIMEOUT 1000000000

#define GPU_SCRATCH_POOL_SIZE (64 * 1024 * 1024)
#define GPU_VRAM_POOL_SIZE    (128 * 1024 * 1024)
#define GPU_STAGING_POOL_SIZE (16 * 1024 * 1024) // NOTE: This comes out of GPU_SCRATCH_POOL_SIZE
static_assert(GPU_STAGING_POOL_SIZE <= GPU_SCRATCH_POOL_SIZE, "");

#define WITH_LOGGING 1

/* Deletion Queue Notes:
 * 
 * So far, all of Vulkan's destroy calls have the same sort of signature,
 * with a device, handle, and allocator callbacks.
 * All handles on this platform at least are defined to be pointers to opaque structs,
 * so type punning the destroy function with void * is not a problem.
 * This is something that needs to be verified on other platforms as well, at a later date.
 */
typedef void (*VK_Destroy_Func)(VkDevice device, void *handle, const VkAllocationCallbacks *pAllocator);

struct VK_Deletion_Entry {
    VK_Destroy_Func func;
    void *handle;
};

struct VK_Deletion_Queue {
    // TODO: This should chain to another block or use a dynamic allocation
    struct VK_Deletion_Entry entries[4096];
    uint32_t entries_top;
};

struct VK_Buffer {
    VkBuffer handle;
    size_t offset;
    size_t size;
};

struct VK_Mem_Arena {
    VkDeviceMemory allocation;
    size_t top;
    size_t capacity;
};

struct VK_Buffer_Arena {
    struct VK_Buffer buffer;
    size_t top;
    size_t capacity;
};

struct VK_Staging_Queue {
    struct {
        uint64_t offset_in_staging_buffer;
        uint64_t size;
        VkBuffer destination_buffer;
    } entries[512];
    uint32_t entries_top;
};

struct Mesh {
    struct VK_Buffer vert_buf;
    struct VK_Buffer index_buf;
    uint32_t vert_count;
    uint32_t index_count;
};

struct VK {
	/* Instances and Handles */
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;

	/* Presentation */
	VkSurfaceKHR surface;
	VkFormat swapchain_format;
	VkFormat depth_format;
	VkExtent2D swapchain_extent;
	VkSwapchainKHR swapchain;

	VkRenderPass render_pass;
	VkImage depth_image;
	VkImageView depth_image_view;
	VkImage swapchain_images[32];
	VkImageView swapchain_image_views[32];
	VkFramebuffer framebuffers[32];
	uint32_t swapchain_image_count;

	/* Queues and Commands */
	VkQueue queue_graphics;

	VkCommandPool command_pool_upload;
	VkCommandPool command_pool_graphics;
	VkCommandBuffer command_buffer_graphics;

	uint32_t queue_graphics_idx;

	/* Synchronization */
	VkSemaphore present_semaphore;
	VkSemaphore render_semaphore;
	VkFence render_fence;
	VkFence upload_fence;

	/* Memory */
	int mem_host_coherent_idx;
	int mem_gpu_local_idx;

	struct VK_Mem_Arena scratch_mem; // TODO CLEANUP: Rename this to arena_scratch_mem
	struct VK_Mem_Arena gpu_mem;

	struct VK_Buffer_Arena staging_buffer;
    struct VK_Staging_Queue staging_queue;

	/* Descriptor */
	VkDescriptorPool desc_pool;

	/* Resources */
	struct VK_Deletion_Queue deletion_queue;

	// -- TODO: Split out these app-specific things
    /* Pipeline and Shaders */
    VkPipelineLayout simple_piepline_layout;
    VkPipeline flat_pipeline;
    VkPipeline lit_pipeline;

    /* Vertex buffers and mesh data */
    struct Mesh meshes[512];
    int mesh_count;
    
    /* Descriptors */
    VkDescriptorSet global_desc;
    VkDescriptorSetLayout global_desc_layout;
    
    /* Buffers */
    struct VK_Buffer global_uniform_buffer;
    // --
};

struct Render_State {
	uint64_t frame_number;
	int mesh_idx;
	bool unlit_shader;
};

struct Push_Constant_Data {
	mat4s model_matrix;
};

struct Global_Uniform_Data {
    mat4s view_mat;
    mat4s proj_mat;
    mat4s view_proj_mat;
};

static struct VK s_vk;
static struct Render_State s_render_state;
static SDL_Window *s_window;

#define countof(x) (sizeof(x) / sizeof(x[0]))

/*
#define MIN(a_, b_) ((a_) < (b_) ? (a_) : (b_))
#define MAX(a_, b_) ((a_) > (b_) ? (a_) : (b_))
#define CLAMP(v_, min_, max_) (MAX(min_, MIN(v_, max_)))
*/

#if WITH_LOGGING
	#define LOG(...) printf(__VA_ARGS__)
#else
	#define LOG(...)
#endif

#define VK_CHECK(x_) \
	do {\
		VkResult err = x_;\
		if(err != VK_SUCCESS) {\
			char buf[128];\
			snprintf(buf, sizeof(buf), "Vulkan error %d at line %d\n", err, __LINE__);\
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Vulkan Error", buf, NULL);\
			abort();\
		}\
	} while(0);

#define CHECK(x_, msg_) do { if(!(x_)) { panic(msg_); } } while(0);
static void panic(const char *message)
{
	fprintf(stderr, "%s\n", message);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Critical Error", message, NULL);
	abort();
}

static uint64_t align_address(uint64_t addr, uint64_t alignment)
{
	return (addr + alignment - 1) & ~(alignment - 1);
}

// NOTE: Allocates with malloc, must free
static char *file_load_binary(const char *path, uint32_t *size)
{
	FILE *fp = fopen(path, "rb");
	if(!fp) {
		fprintf(stderr, "File open error: Couldn't open %s\n", path);
		return NULL;
	}

	fseek(fp, 0, SEEK_END);
	*size = ftell(fp);
	rewind(fp);

	char *buf = malloc(*size);
	fread(buf, 1, *size, fp);

	LOG("Loaded file from: %s\n", path);

	return buf;
}

static VkShaderModule vk_create_shader_module_from_file(struct VK *vk, const char *path)
{
	uint32_t size;
	char *code = file_load_binary(path, &size);
	CHECK(code, "Couldn't load shader file");

	VkShaderModuleCreateInfo create_info = {
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = (uint32_t *)code
	};

	VkShaderModule module;
	VK_CHECK(vkCreateShaderModule(vk->device, &create_info, NULL, &module));

	free(code); // TODO: Must check if it's OK to free after calling above function

	LOG("Created shader from: %s\n", path);
	
	return module;
}

static void vk_push_deletable(struct VK *vk, void (*func)(), void *handle)
{
	CHECK(vk->deletion_queue.entries_top < countof(vk->deletion_queue.entries), "Ran out of slots on deletion queue");
	
	vk->deletion_queue.entries[vk->deletion_queue.entries_top++] = (struct VK_Deletion_Entry) {
		.func = func,
		.handle = handle
	};
}

static struct VK_Mem_Arena vk_alloc_mem_arena(struct VK *vk, int memory_type_idx, size_t capacity)
{
    VkMemoryAllocateInfo alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = capacity,
        .memoryTypeIndex = memory_type_idx,
    };

    VkDeviceMemory allocation;
    
    VK_CHECK(vkAllocateMemory(vk->device, &alloc_info, NULL, &allocation));
    vk_push_deletable(vk, vkFreeMemory, allocation);

    LOG("Created GPU memory arena with size: %.1fKB from memory type: %d\n", (float)capacity / 1024.0f, memory_type_idx);

    return (struct VK_Mem_Arena) {
        .allocation = allocation,
        .capacity = capacity,
        .top = 0
    };
}

static uint64_t vk_mem_arena_push(struct VK *vk, struct VK_Mem_Arena *arena, VkMemoryRequirements mem_req)
{
    arena->top = align_address(arena->top, mem_req.alignment);
    const uint64_t buffer_address = arena->top;
    arena->top += mem_req.size;

    LOG("Push to arena %p size: %.3fKB (%.3f%% usage)\n", arena, (float)mem_req.size / 1024.0f, 100 * ((float)arena->top / (float)arena->capacity));

    return buffer_address;
}

// NOTE: The buffer created here is not pushed to the deletion queue
static struct VK_Buffer vk_create_buffer_ex(struct VK *vk, struct VK_Mem_Arena *arena, VkBufferUsageFlagBits usage, size_t size)
{
    VkBufferCreateInfo buffer_info = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .flags = 0,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE
    };

    VkBuffer buffer;

    VK_CHECK(vkCreateBuffer(vk->device, &buffer_info, NULL, &buffer));

    VkMemoryRequirements mem_requirements;
    vkGetBufferMemoryRequirements(vk->device, buffer, &mem_requirements);

    const size_t buffer_addr = vk_mem_arena_push(vk, arena, mem_requirements);
    vkBindBufferMemory(vk->device, buffer, arena->allocation, buffer_addr);

    return (struct VK_Buffer) {
        .handle = buffer,
        .offset = buffer_addr,
        .size = size // NOTE: Here we do not use mem_requirements.size because that can be bigger than the wanted buffer size
    };
    
    LOG("Created buffer\n");
}

static struct VK_Buffer vk_create_buffer(struct VK *vk, VkBufferUsageFlagBits usage, size_t size)
{
    struct VK_Buffer buffer = vk_create_buffer_ex(vk, &vk->scratch_mem, usage, size);
    vk_push_deletable(vk, vkDestroyBuffer, buffer.handle);
    
    return buffer;
}

static struct VK_Buffer_Arena vk_alloc_buffer_arena(struct VK *vk, struct VK_Mem_Arena *arena, VkBufferUsageFlagBits usage, size_t capacity)
{
    struct VK_Buffer buffer = vk_create_buffer_ex(vk, arena, usage, capacity);
    vk_push_deletable(vk, vkDestroyBuffer, buffer.handle);

    LOG("Created buffer-backed arena with size: %.1fKB\n", (float)capacity / 1024.0f);

    return (struct VK_Buffer_Arena) {
        .buffer = buffer,
        .capacity = capacity,
        .top = 0
    };
}

static uint64_t vk_buffer_arena_push(struct VK *vk, struct VK_Buffer_Arena *arena, VkMemoryRequirements mem_req)
{
    // TODO: Maybe deduplicate with vk_mem_arena_push, the logic is exactly the same
    arena->top = align_address(arena->top, mem_req.alignment);
    const uint64_t buffer_address = arena->top;
    arena->top += mem_req.size;

    LOG("Push to buffer arena %p size: %.3fKB (%.3f%% usage)\n", arena, (float)mem_req.size / 1024.0f, 100 * ((float)arena->top / (float)arena->capacity));

    return buffer_address;
}

static void vk_staging_queue_flush(struct VK *vk)
{
    // TODO SYNC: This function may need to wait on the graphics render if we want to use it outside of init.
    //			  Alternatively or in addition, this can maybe use a different queue. (is that necessary?)

    /* Allocate command buffer and begin command recording */
    VkCommandBufferAllocateInfo cmd_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = vk->command_pool_upload,
        .commandBufferCount = 1,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY
    };

    VkCommandBuffer cmdbuf;
    VK_CHECK(vkAllocateCommandBuffers(vk->device, &cmd_alloc_info, &cmdbuf));

    VkCommandBufferBeginInfo cmd_begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pInheritanceInfo = NULL,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
    };
    VK_CHECK(vkBeginCommandBuffer(cmdbuf, &cmd_begin_info));

    /* Record commands for every staged entry */
    for(uint32_t i = 0; i < vk->staging_queue.entries_top; ++i) {
        struct VkBufferCopy buffer_copy = {
            .srcOffset = vk->staging_queue.entries[i].offset_in_staging_buffer,
            .dstOffset = 0,
            .size = vk->staging_queue.entries[i].size
        };


        vkCmdCopyBuffer(cmdbuf, vk->staging_buffer.buffer.handle, vk->staging_queue.entries[i].destination_buffer, 1, &buffer_copy);
    }

    VK_CHECK(vkEndCommandBuffer(cmdbuf));

    /* Submit copy commands */
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pCommandBuffers = &cmdbuf,
        .commandBufferCount = 1
    };

    VK_CHECK(vkQueueSubmit(vk->queue_graphics, 1, &submit_info, vk->upload_fence));

    vkWaitForFences(vk->device, 1, &vk->upload_fence, true, UINT64_MAX);
    vkResetFences(vk->device, 1, &vk->upload_fence);
    vkResetCommandPool(vk->device, vk->command_pool_upload, 0);

    vk->staging_buffer.top = 0;
    vk->staging_queue.entries_top = 0;

    LOG("Finished all pending staging buffer uploads\n");
}

// NOTE: With WITH_GROUPED_STAGING_TRANSFER on, the buffer returned is not usable until vk_staging_queue_flush is called.
static struct VK_Buffer vk_create_and_upload_buffer(struct VK *vk, VkBufferUsageFlagBits usage, const void *data, size_t size)
{
    /* flush the staging queue if we can't fit any more */
    const bool staging_buffer_full = size > vk->staging_buffer.capacity - vk->staging_buffer.top;
    const bool staging_queue_full = vk->staging_queue.entries_top == countof(vk->staging_queue.entries);
    if(staging_buffer_full || staging_queue_full) {
        vk_staging_queue_flush(vk);
    }

    assert(size < vk->staging_buffer.capacity - vk->staging_buffer.top);

	/* create the real buffer */
	struct VK_Buffer buffer = vk_create_buffer_ex(vk, &vk->gpu_mem, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, size);
	vk_push_deletable(vk, vkDestroyBuffer, buffer.handle);	

	VkMemoryRequirements mem_req;
	vkGetBufferMemoryRequirements(vk->device, buffer.handle, &mem_req);
	const uint64_t staging_buffer_offset = vk_buffer_arena_push(vk, &vk->staging_buffer, mem_req);
	
	// TODO: Couple staging buffer and allocation as well, we shouldn't guess that it's in vk->scratch_mem!!
	void *mapped_mem;
	VK_CHECK(vkMapMemory(vk->device, vk->scratch_mem.allocation, vk->staging_buffer.buffer.offset + staging_buffer_offset, buffer.size, 0, &mapped_mem));
	memcpy(mapped_mem, data, size);
	vkUnmapMemory(vk->device, vk->scratch_mem.allocation);

    vk->staging_queue.entries[vk->staging_queue.entries_top].destination_buffer = buffer.handle;
    vk->staging_queue.entries[vk->staging_queue.entries_top].size = buffer.size;
    vk->staging_queue.entries[vk->staging_queue.entries_top].offset_in_staging_buffer = staging_buffer_offset;

    vk->staging_queue.entries_top++;

    LOG("Created buffer, uploaded to staging buffer. Queued staging transfer for later.\n");

	return buffer;
}

// TODO: Expose more options as parameters
static VkPipeline vk_create_pipeline(struct VK *vk,
                                     VkPipelineLayout layout,
                                     VkPipelineShaderStageCreateInfo *shader_stages,
                                     int shader_stage_count)
{
    VkVertexInputBindingDescription binding_descs[] = {
        {
            .binding = 0,
            .stride = sizeof(float) * 8,
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX
        },
    };
    
    VkVertexInputAttributeDescription attr_descs[] = {
        {
            .location = 0,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = 0
        },
        {
            .location = 1,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32_SFLOAT,
            .offset = sizeof(float) * 3
        },
        {
            .location = 2,
            .binding = 0,
            .format = VK_FORMAT_R32G32B32A32_SFLOAT,
            .offset = sizeof(float) * 6
        }
    };

    VkPipelineVertexInputStateCreateInfo vertex_input_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        // Fill these in later
        .vertexBindingDescriptionCount = countof(binding_descs),
        .pVertexBindingDescriptions = binding_descs,
        .vertexAttributeDescriptionCount = countof(attr_descs),
        .pVertexAttributeDescriptions = attr_descs,
    };

    VkPipelineInputAssemblyStateCreateInfo input_assembly_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE
    };

    VkViewport viewport = {
        .x = 0.0f,
        .y = 0.0f,
        .width = vk->swapchain_extent.width,
        .height = vk->swapchain_extent.height,
        .minDepth = 0.0f,
        .maxDepth = 1.0f
    };

    VkRect2D scissor = {
        .offset = {0, 0},
        .extent = vk->swapchain_extent
    };

    VkPipelineViewportStateCreateInfo viewport_state_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor
    };

    VkPipelineRasterizationStateCreateInfo rasterizer_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .lineWidth = 1.0f, // NOTE: Anything thicker than 1 needs a capability enabled
        //.cullMode = VK_CULL_MODE_BACK_BIT,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f
    };

    VkPipelineMultisampleStateCreateInfo multisampling_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .sampleShadingEnable = VK_FALSE,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .minSampleShading = 1.0f,
        .pSampleMask = NULL,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE
    };

    VkPipelineColorBlendAttachmentState color_blend_attachment = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
        .blendEnable = VK_FALSE,
        /*
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD
        */
    };

    VkPipelineColorBlendStateCreateInfo color_blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
    };

    VkPipelineDepthStencilStateCreateInfo depth_stencil = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        .depthBoundsTestEnable = VK_FALSE, // TODO: Check what this does
        .minDepthBounds = 0.0f,
        .maxDepthBounds = 1.0f,
        .stencilTestEnable = VK_FALSE
    };

    VkGraphicsPipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = shader_stage_count,
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly_info,
        .pViewportState = &viewport_state_info,
        .pRasterizationState = &rasterizer_info,
        .pMultisampleState = &multisampling_info,
        .pColorBlendState = &color_blending,
        .pDepthStencilState = &depth_stencil,
        .layout = layout,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .renderPass = vk->render_pass // TODO: Pass this in
    };

    VkPipeline pipeline;
    VK_CHECK(vkCreateGraphicsPipelines(vk->device, VK_NULL_HANDLE, 1, &pipeline_info, NULL, &pipeline));
    vk_push_deletable(vk, vkDestroyPipeline, pipeline);

    LOG("Created pipeline\n");

    return pipeline;
}

static VkPipeline vk_create_pipeline_and_shaders(struct VK *vk,
                                           const char *vert_path,
                                           const char *frag_path,
                                           VkPipelineLayout layout)
{
    assert(frag_path && vert_path);

    VkShaderModule shader_vert = vk_create_shader_module_from_file(vk, vert_path);
    VkShaderModule shader_frag = vk_create_shader_module_from_file(vk, frag_path);

    VkPipelineShaderStageCreateInfo shader_stages[] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = shader_vert,
            .pName = "main"
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = shader_frag,
            .pName = "main"
        }
    };

    /* pipeline */
    VkPipeline pipeline = vk_create_pipeline(vk, layout, shader_stages, countof(shader_stages));

    vkDestroyShaderModule(vk->device, shader_frag, NULL);
    vkDestroyShaderModule(vk->device, shader_vert, NULL);
    
    return pipeline;
}

static void vk_init(struct VK *vk)
{
	/* instance */
	{
		/* extensions */
		const char *extension_names[256];
		uint32_t extension_count = countof(extension_names);

		SDL_Vulkan_GetInstanceExtensions(s_window, &extension_count, extension_names);

		/* layers */
		VkLayerProperties available_layers[256];
		uint32_t available_layer_count = countof(available_layers);

		vkEnumerateInstanceLayerProperties(&available_layer_count, available_layers);

		const char *layer_names[] = {
			"VK_LAYER_KHRONOS_validation"
		};
		uint32_t layer_count = countof(layer_names);

		static_assert(countof(layer_names) == 1, "Edit the code below to support more");
		bool found = false;
		for(uint32_t i = 0; i < available_layer_count; ++i) {
			if(0 == strcmp(available_layers[i].layerName, layer_names[0])) {
				found = true;
				break;
			}
		}

		if(!found) {
			layer_count = 0;
		}

		/* instance */
		VkInstanceCreateInfo instance_create_info = {
			.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
			.enabledLayerCount = layer_count,
			.ppEnabledLayerNames = layer_names,
			.enabledExtensionCount = extension_count,
			.ppEnabledExtensionNames = extension_names
		};

		VK_CHECK(vkCreateInstance(&instance_create_info, NULL, &vk->instance));
	}

	/* physical device */
	{
		VkPhysicalDevice devices[256];
		uint32_t device_count = countof(devices);

		vkEnumeratePhysicalDevices(vk->instance, &device_count, devices);
		CHECK(device_count, "No GPUs found");

        int target_index = 0;
        for(int i = 0; i < device_count; ++i) {
            VkPhysicalDeviceProperties props;
            vkGetPhysicalDeviceProperties(devices[i], &props);

            if(props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
                target_index = i;
                break;
            }
        }

		vk->physical_device = devices[target_index];
	}

    /* memory query */
    {
        VkPhysicalDeviceMemoryProperties mem_properties;
        vkGetPhysicalDeviceMemoryProperties(vk->physical_device, &mem_properties);
    
        /* print info */
        printf("Memory heaps:\n");
        for(uint32_t i = 0; i < mem_properties.memoryHeapCount; ++i) {
            printf("-> [%d] %zuMB\n", i, mem_properties.memoryHeaps[i].size / (1024 * 1024));
        }
        printf("\n");

        printf("Memory types:\n");
        for(uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
            printf("-> [%d] Index: %d Flags:", i, mem_properties.memoryTypes[i].heapIndex);
            int flags = mem_properties.memoryTypes[i].propertyFlags;
            if(flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                printf("DEVICE_LOCAL ");
            if(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
                printf("HOST_VISIBLE ");
            if(flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                printf("HOST_COHERENT ");
            if(flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                printf("HOST_CACHED ");
            if(flags & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT)
                printf("LAZILY_ALLOCATED ");
            printf("\n");
        }
        printf("\n");
        
        /* Choose a memory type that is host visible */
        printf("Searching HOST_VISIBLE | HOST_COHERENT | HOST_CACHED, memory heap\n");
        bool found = false;
        for(uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
            if(mem_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT &&
               mem_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT &&
               mem_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT
            ) {
                vk->mem_host_coherent_idx = i;
                found = true;
                break;
            }
        }

        if(!found) {
            printf("Falling back to un-cached HOST_VISIBLE | HOST_COHERENT memory heap\n");

            for(uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
                if(mem_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT &&
                   mem_properties.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                ) {
                    vk->mem_host_coherent_idx = i;
                    found = true;
                    break;
                }
            }            
        }

        CHECK(found, "Couldn't find host visible and coherent memory heap");
        printf("-> Chose type %d (heap %d)\n", vk->mem_host_coherent_idx, mem_properties.memoryTypes[vk->mem_host_coherent_idx].heapIndex);
        
        /* Choose a memory type that is fast */
        printf("Searching DEVICE_LOCAL and not HOST_VISIBLE memory heap\n");
        found = false;
        for(uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
            if(mem_properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) &&
               !(mem_properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            ) {
                vk->mem_gpu_local_idx = i;
                found = true;
                break;
            }
        }

        if(!found) {
            printf("Falling back to any DEVICE_LOCAL, even if HOST_VISIBLE (is this an integrated card?)\n");
            for(uint32_t i = 0; i < mem_properties.memoryTypeCount; ++i) {
                if(mem_properties.memoryTypes[i].propertyFlags & (VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    vk->mem_gpu_local_idx = i;
                    found = true;
                    break;
                }
            }   
        }

        CHECK(found, "Couldn't find device local memory");
        printf("-> Chose type %d (heap %d)\n\n", vk->mem_gpu_local_idx, mem_properties.memoryTypes[vk->mem_gpu_local_idx].heapIndex);
    }

	/* surface */
	{
		// TODO: Make sure we are allowed to have this before VkDevice creation
		CHECK(SDL_Vulkan_CreateSurface(s_window, vk->instance, &vk->surface), "Couldn't create surface");
	}

	/* queues query */
	{
		VkQueueFamilyProperties queue_families[32];
		uint32_t queue_families_count = countof(queue_families);
		vkGetPhysicalDeviceQueueFamilyProperties(vk->physical_device, &queue_families_count, queue_families);

		bool found_graphics = false;
		for(uint32_t i = 0; i < queue_families_count; ++i) {
			/* :NOTE:
			 * Unlike vulkan-tutorial, we find combined present/graphics queues because
			 * drivers that don't support this do not seem to exist.
			 */
			VkBool32 present_support = false;
			vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device, i, vk->surface, &present_support);
			if(!found_graphics && present_support && queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				found_graphics = true;
				vk->queue_graphics_idx = i;
			}
		}

		CHECK(found_graphics, "No combined graphics/present queue found");
	}

	/* logical device */
	{
		/* extensions */
		const char *extension_names[] = {
			VK_KHR_SWAPCHAIN_EXTENSION_NAME
		};
		uint32_t extension_count = countof(extension_names);

		VkExtensionProperties supported_extensions[1024];
		uint32_t supported_extension_count = countof(supported_extensions);
		vkEnumerateDeviceExtensionProperties(vk->physical_device, NULL, &supported_extension_count, supported_extensions);

		for(uint32_t i = 0; i < extension_count; ++i) {
			bool found = false;
			for(uint32_t j = 0; j < supported_extension_count; ++j) {
				if(0 == strcmp(extension_names[i], supported_extensions[j].extensionName)) {
					found = true;
					break;
				}
			}

			CHECK(found, "Didn't find all required extensions");
		}

		/* queue */
		VkDeviceQueueCreateInfo queue_infos[1];
		uint32_t queue_indices[1] = {
			vk->queue_graphics_idx,
		};

		static_assert(countof(queue_infos) == countof(queue_indices), "");

		uint32_t queue_count = countof(queue_indices);

		float queue_priority = 1.0f;
		for(uint32_t i = 0; i < queue_count; ++i) {
			queue_infos[i] = (VkDeviceQueueCreateInfo) {
				.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
				.queueFamilyIndex = queue_indices[i],
				.queueCount = 1,
				.pQueuePriorities = &queue_priority,
			};
		}

		/* device features */
        //VkPhysicalDeviceFeatures device_features = {0};

		/* create */
		VkDeviceCreateInfo create_info = {
			.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
			.queueCreateInfoCount = queue_count,
			.pQueueCreateInfos = queue_infos,
			.enabledExtensionCount = extension_count,
			.ppEnabledExtensionNames = extension_names
		};

		VK_CHECK(vkCreateDevice(vk->physical_device, &create_info, NULL, &vk->device));

		vkGetDeviceQueue(vk->device, vk->queue_graphics_idx, 0, &vk->queue_graphics);
	}

    /* memory allocation */
    {
        vk->scratch_mem = vk_alloc_mem_arena(vk, vk->mem_host_coherent_idx, GPU_SCRATCH_POOL_SIZE);
        vk->gpu_mem = vk_alloc_mem_arena(vk, vk->mem_gpu_local_idx, GPU_VRAM_POOL_SIZE);
        vk->staging_buffer = vk_alloc_buffer_arena(vk, &vk->scratch_mem, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, GPU_STAGING_POOL_SIZE);
    }

	/* swap chain */
	{
		/* query */
		VkSurfaceFormatKHR supported_formats[256];
		uint32_t supported_formats_count = countof(supported_formats);
		vkGetPhysicalDeviceSurfaceFormatsKHR(vk->physical_device, vk->surface, &supported_formats_count, supported_formats);

		VkPresentModeKHR supported_modes[256];
		uint32_t support_modes_count = countof(supported_modes);
		vkGetPhysicalDeviceSurfacePresentModesKHR(vk->physical_device, vk->surface, &support_modes_count, supported_modes);

		VkSurfaceCapabilitiesKHR capabilities;
		vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk->physical_device, vk->surface, &capabilities);

		int width, height;
		SDL_Vulkan_GetDrawableSize(s_window, &width, &height);
        VkExtent2D extent = {width, height};
        vk->swapchain_extent = extent;

		/* find */
		VkSurfaceFormatKHR format;
		bool format_found = false;
		for(uint32_t i = 0; i < supported_formats_count; ++i) {
			if(supported_formats[i].format == VK_FORMAT_B8G8R8A8_SRGB &&
			   supported_formats[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			{
				format = supported_formats[i];
				format_found = true;
				break;
			}
		}
		CHECK(format_found, "Couldn't find a suitable surface format");

		VkPresentModeKHR mode;
		bool mode_found = false;
		for(uint32_t i = 0; i < support_modes_count; ++i) {
			if(supported_modes[i] == VK_PRESENT_MODE_FIFO_KHR) {
				mode = supported_modes[i];
				mode_found = true;
				break;
			}
		}
		CHECK(mode_found, "Couldn't find a suitable present mode");

		/* create */
		uint32_t image_count = capabilities.minImageCount;
		CHECK(image_count < countof(vk->swapchain_image_views), "Minimum swapchain image count is too high");

		VkExtent3D depth_image_extent = {
			vk->swapchain_extent.width,
			vk->swapchain_extent.height,
			1
		};

		vk->depth_format = VK_FORMAT_D32_SFLOAT;

		VkImageCreateInfo depth_image_create_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
			.imageType = VK_IMAGE_TYPE_2D,
			.format = vk->depth_format,
			.extent = depth_image_extent,
			.mipLevels = 1,
			.arrayLayers = 1,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.tiling = VK_IMAGE_TILING_OPTIMAL,
			.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
		};	

		VK_CHECK(vkCreateImage(vk->device, &depth_image_create_info, NULL, &vk->depth_image));
		vk_push_deletable(vk, vkDestroyImage, vk->depth_image);

        VkMemoryRequirements depth_image_mem_requirements;
        vkGetImageMemoryRequirements(vk->device, vk->depth_image, &depth_image_mem_requirements);

        VkMemoryAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = depth_image_mem_requirements.size,
            .memoryTypeIndex = vk->mem_gpu_local_idx
        };

        const uint64_t buffer_address = vk_mem_arena_push(vk, &vk->gpu_mem, depth_image_mem_requirements);
        vkBindImageMemory(vk->device, vk->depth_image, vk->gpu_mem.allocation, buffer_address);

		VkImageViewCreateInfo depth_image_view_create_info = {
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.image = vk->depth_image,
			.format = vk->depth_format,
			.subresourceRange = {
				.baseMipLevel = 0,
				.levelCount = 1,
				.baseArrayLayer = 0,
				.layerCount = 1,
				.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT
			}
		};

		VK_CHECK(vkCreateImageView(vk->device, &depth_image_view_create_info, NULL, &vk->depth_image_view));
		vk_push_deletable(vk, vkDestroyImageView, vk->depth_image_view);

		VkSwapchainCreateInfoKHR create_info = {
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.surface = vk->surface,
			.minImageCount = image_count,
			.imageFormat = format.format,
			.imageColorSpace = format.colorSpace,
			.imageExtent = extent,
			.imageArrayLayers = 1,
			.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
			.preTransform = capabilities.currentTransform,
			.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
			.presentMode = mode,
			.clipped = VK_TRUE,
			.oldSwapchain = VK_NULL_HANDLE,
			.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE // NOTE: This would be VK_SHARING_MODE_CONCURRENT for separate present/graphics queue but we don't support that
		};

		VK_CHECK(vkCreateSwapchainKHR(vk->device, &create_info, NULL, &vk->swapchain));
		vk_push_deletable(vk, vkDestroySwapchainKHR, vk->swapchain);
		
        // NOTE: There is a warning on Intel GPUs that suggests this function does actually want to be called twice
		vk->swapchain_image_count = image_count;
        vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &vk->swapchain_image_count, NULL);
		VK_CHECK(vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &vk->swapchain_image_count, vk->swapchain_images));

		vk->swapchain_format = format.format;
	}

	/* swapchain image views */
	{
		for(uint32_t i = 0; i < vk->swapchain_image_count; ++i) {
			VkImageViewCreateInfo create_info = {
				.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
				.image = vk->swapchain_images[i],
				.viewType = VK_IMAGE_VIEW_TYPE_2D,
				.format = vk->swapchain_format,
				.components = { VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
				.subresourceRange = {
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.baseMipLevel = 0,
					.levelCount = 1,
					.baseArrayLayer = 0,
					.layerCount = 1
				}
			};

			VK_CHECK(vkCreateImageView(vk->device, &create_info, NULL, &vk->swapchain_image_views[i]));
			vk_push_deletable(vk, vkDestroyImageView, vk->swapchain_image_views[i]);
		}
	}

	/* commands */
	{
		/* graphics pool */
		VkCommandPoolCreateInfo graphics_pool_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.queueFamilyIndex = vk->queue_graphics_idx,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT // allow for resetting individual command buffers, but do we need it?
		};

		VK_CHECK(vkCreateCommandPool(vk->device, &graphics_pool_info, NULL, &vk->command_pool_graphics));
		vk_push_deletable(vk, vkDestroyCommandPool, vk->command_pool_graphics);

		/* upload pool */
		VkCommandPoolCreateInfo upload_pool_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.queueFamilyIndex = vk->queue_graphics_idx,
			.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT
		};

		VK_CHECK(vkCreateCommandPool(vk->device, &graphics_pool_info, NULL, &vk->command_pool_upload));
		vk_push_deletable(vk, vkDestroyCommandPool, vk->command_pool_upload);
		
		/* graphics buffer */
		VkCommandBufferAllocateInfo command_alloc_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = vk->command_pool_graphics,
			.commandBufferCount = 1,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY
		};

		VK_CHECK(vkAllocateCommandBuffers(vk->device, &command_alloc_info, &vk->command_buffer_graphics));
	}

    /* render pass */
    {
        VkAttachmentDescription attachments[] = {
            {
                // Colour
                .format = vk->swapchain_format,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
            },
            {
                // Depth
                .format = vk->depth_format,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
            }
        };
        
        VkAttachmentReference color_attachment_ref = {
            .attachment = 0, // References the pAttachments array in the parent renderpass
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };

        VkAttachmentReference depth_attachment_ref = {
            .attachment = 1,
            .layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
        };

        VkSubpassDescription subpass = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_attachment_ref,
            .pDepthStencilAttachment = &depth_attachment_ref
        };
       
        VkRenderPassCreateInfo render_pass_info = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = countof(attachments),
            .pAttachments = attachments, // This is where VkAttachmentReference::attachment indexes into
            .subpassCount = 1,
            .pSubpasses = &subpass
        };

        VK_CHECK(vkCreateRenderPass(vk->device, &render_pass_info, NULL, &vk->render_pass));
        vk_push_deletable(vk, vkDestroyRenderPass, vk->render_pass);
    }

    /* descriptors */
    {
        // TODO: Make a function for creating this too, since it's app-specific
        /* descriptor layouts */
        VkDescriptorSetLayoutBinding bindings[] = {
            {
                .binding = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
            }
        };

        VkDescriptorSetLayoutCreateInfo desc_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = countof(bindings),
            .pBindings = bindings,
            .flags = 0
        };

        VK_CHECK(vkCreateDescriptorSetLayout(vk->device, &desc_info, NULL, &vk->global_desc_layout));
        vk_push_deletable(vk, vkDestroyDescriptorSetLayout, vk->global_desc_layout);

        /* descriptor pool */
        VkDescriptorPoolSize sizes[] = {
            { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 10 }
        };

        VkDescriptorPoolCreateInfo pool_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags = 0,
            .maxSets = 10,
            .poolSizeCount = countof(sizes),
            .pPoolSizes = sizes
        };
        
        VK_CHECK(vkCreateDescriptorPool(vk->device, &pool_info, NULL, &vk->desc_pool));
        vk_push_deletable(vk, vkDestroyDescriptorPool, vk->desc_pool);

        /* descriptors */
        // NOTE: The actual buffer is created way after in scene_init, so this just allocates the descriptor for use later
        VkDescriptorSetAllocateInfo alloc_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = vk->desc_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &vk->global_desc_layout,
        };
        
        VK_CHECK(vkAllocateDescriptorSets(vk->device, &alloc_info, &vk->global_desc));
    }

	/* pipeline layout */
	{
		// TODO: Make a function for creating this too, since it's app-specific
		VkPushConstantRange ranges[] = {
			{
				.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				.offset = 0,
				.size = sizeof(struct Push_Constant_Data)
			}
		};

		VkPipelineLayoutCreateInfo pipeline_layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &vk->global_desc_layout,
            .pushConstantRangeCount = countof(ranges),
            .pPushConstantRanges = ranges
        };

        VK_CHECK(vkCreatePipelineLayout(vk->device, &pipeline_layout_info, NULL, &vk->simple_piepline_layout));
        vk_push_deletable(vk, vkDestroyPipelineLayout, vk->simple_piepline_layout);
    }

    /* framebuffers */
    {
        VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = vk->render_pass,
            .width = vk->swapchain_extent.width,
            .height = vk->swapchain_extent.height,
            .layers = 1
        };

        for(uint32_t i = 0; i < vk->swapchain_image_count; ++i) {
            VkImageView attachments[] = {
                vk->swapchain_image_views[i],
                vk->depth_image_view
            };
            
            framebuffer_info.pAttachments = attachments;
            framebuffer_info.attachmentCount = countof(attachments);

            VK_CHECK(vkCreateFramebuffer(vk->device, &framebuffer_info, NULL, &vk->framebuffers[i]));
            vk_push_deletable(vk, vkDestroyFramebuffer, vk->framebuffers[i]);
        }
    }

    /* synchronization */
    {
        /* render fence */
        {
            VkFenceCreateInfo fence_info = {
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
            };
    
            VK_CHECK(vkCreateFence(vk->device, &fence_info, NULL, &vk->render_fence));
            vk_push_deletable(vk, vkDestroyFence, vk->render_fence);
        }

        /* upload fence */
        {
            VkFenceCreateInfo fence_info = {
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            };
    
            VK_CHECK(vkCreateFence(vk->device, &fence_info, NULL, &vk->upload_fence));
            vk_push_deletable(vk, vkDestroyFence, vk->upload_fence);
        }

        /* semaphores */        
        VkSemaphoreCreateInfo semaphore_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .flags = 0
        };

        VK_CHECK(vkCreateSemaphore(vk->device, &semaphore_info, NULL, &vk->present_semaphore));
        VK_CHECK(vkCreateSemaphore(vk->device, &semaphore_info, NULL, &vk->render_semaphore));
        vk_push_deletable(vk, vkDestroySemaphore, vk->present_semaphore);
        vk_push_deletable(vk, vkDestroySemaphore, vk->render_semaphore);
    }
    
    LOG("vk_init done\n");
}

static void vk_destroy(struct VK *vk)
{
    vkDeviceWaitIdle(vk->device);

    for(int i = vk->deletion_queue.entries_top - 1; i >= 0; --i) {
        vk->deletion_queue.entries[i].func(vk->device, vk->deletion_queue.entries[i].handle, NULL);
    }

	vkDestroySurfaceKHR(vk->instance, vk->surface, NULL);
	vkDestroyDevice(vk->device, NULL);
	vkDestroyInstance(vk->instance, NULL);
	
	LOG("vk_destroy done\n");
}

static void vk_update_buffer(struct VK *vk, struct VK_Buffer buf, void *data, size_t offset, size_t size)
{
    assert(offset + size <= buf.size);
    
    void *mapped_mem;
    vkMapMemory(vk->device, vk->scratch_mem.allocation, buf.offset, buf.size, 0, &mapped_mem);
    memcpy((char *)mapped_mem + offset, data, size);
    vkUnmapMemory(vk->device, vk->scratch_mem.allocation);
}

static struct Mesh upload_mesh_from_raw_data(struct VK *vk, const char *mesh_data)
{
    const size_t vert_buffer_stride = 8;
    const size_t vert_buffer_stride_bytes = vert_buffer_stride * sizeof(float);

    const char *p = mesh_data;
    uint32_t vert_count = *(uint32_t *)p;
    p += sizeof(vert_count);

    uint32_t index_count = *(uint32_t *)p;
    p += sizeof(index_count);

    const size_t vert_buffer_size = vert_count * vert_buffer_stride_bytes;
    const size_t index_buffer_size = index_count * sizeof(uint16_t);

    const float *vert_buffer_data = (float *)p;
    p += vert_buffer_size;

    const uint16_t *index_buffer_data = (uint16_t *)p;

    struct Mesh mesh = {
        .vert_count = vert_count,
        .index_count = index_count
    };

    mesh.vert_buf = vk_create_and_upload_buffer(vk, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, vert_buffer_data, vert_buffer_size);
    mesh.index_buf = vk_create_and_upload_buffer(vk, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, index_buffer_data, index_buffer_size);

    LOG("Uploaded mesh from raw data\n");
    
    return mesh;
}

static void scene_init(struct Render_State *r, struct VK *vk)
{
	vk->flat_pipeline = vk_create_pipeline_and_shaders(vk, "shaders/flat_vert.spv", "shaders/flat_frag.spv", vk->simple_piepline_layout);
	vk->lit_pipeline = vk_create_pipeline_and_shaders(vk, "shaders/lit_vert.spv", "shaders/lit_frag.spv", vk->simple_piepline_layout);

    /* Geometry init */
    {
        const char *mesh_paths[] = {
            "data/suzanne.bin",
            "data/cube.bin"
        };

        for(int i = 0; i < countof(mesh_paths); ++i) {
            uint32_t file_size;
   
            char *mesh_data = file_load_binary(mesh_paths[i], &file_size);
            vk->meshes[vk->mesh_count++] = upload_mesh_from_raw_data(vk, mesh_data);
            free(mesh_data);            
        }

        vk_staging_queue_flush(vk);
    }

    /* Uniform buffer init */
    {
        vk->global_uniform_buffer = vk_create_buffer(vk, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, sizeof(struct Global_Uniform_Data));
        
        VkDescriptorBufferInfo desc_buf_info = {
            .buffer = vk->global_uniform_buffer.handle,
            .offset = 0,
            .range = sizeof(struct Global_Uniform_Data),
        };
        
        VkWriteDescriptorSet set_write = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = 0,
            .dstSet = vk->global_desc,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .pBufferInfo = &desc_buf_info
        };
        
        vkUpdateDescriptorSets(vk->device, 1, &set_write, 0, NULL);
    }
    
    LOG("Scene init done\n");
}

static void render(struct Render_State *r, struct VK *vk)
{
	/* sync */
	VK_CHECK(vkWaitForFences(vk->device, 1, &vk->render_fence, true, TIMEOUT));
	VK_CHECK(vkResetFences(vk->device, 1, &vk->render_fence));

	/* SYNC: Here we pass in a semaphore that will be signalled once we have an
	 * image available to draw into.
     *
     * NOTE: The timeout is set to infinite (UINT64_MAX) because otherwise on Wayland/AMD
     * the window being completely covered will cause it to not give us a next image.	
     * This effectively pauses the application until it's visible again.
	*/
	uint32_t swapchain_index;
    VK_CHECK(vkAcquireNextImageKHR(vk->device, vk->swapchain, UINT64_MAX, vk->present_semaphore, NULL, &swapchain_index));

	/* commands */
	VK_CHECK(vkResetCommandBuffer(vk->command_buffer_graphics, 0));

	VkCommandBuffer cmdbuf = vk->command_buffer_graphics;

	VkCommandBufferBeginInfo cmdbuf_begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.pInheritanceInfo = NULL,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};

	VK_CHECK(vkBeginCommandBuffer(cmdbuf, &cmdbuf_begin_info));

	VkClearValue clear_values[] = {
		{ .color = {0} },
		{ .depthStencil = {.depth = 1.0f} }
	};

	VkRenderPassBeginInfo render_pass_info = {
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = vk->render_pass,
		.renderArea = {
			.offset = {0},
			.extent = vk->swapchain_extent
		},
		.framebuffer = vk->framebuffers[swapchain_index],
		.clearValueCount = countof(clear_values),
		.pClearValues = clear_values
	};

	/* app-specific */
	{
		/* update state and set up data */
		const float flash = fabs(sinf(r->frame_number / 120.0f));
		
		mat4s view = glms_mat4_identity();
		mat4s proj = glms_perspective(glm_rad(70.0f), (float)WIDTH / (float)HEIGHT, 0.1f, 1000.0f);
		proj.raw[1][1] *= -1;

		struct Push_Constant_Data constants = {
			.model_matrix = glms_mat4_identity(),
		};

		const float y = sinf(r->frame_number / 40.0f) * 0.25f;
        constants.model_matrix = glms_translate_make((vec3s){0.0f, y - 0.15f, 3.5f});
		constants.model_matrix = glms_rotate(constants.model_matrix, glm_rad(r->frame_number), (vec3s){0.0f, 1.0f, 0.0f});

		struct Global_Uniform_Data uniforms = {
			.view_mat = view,
			.proj_mat = proj,
			.view_proj_mat = glms_mat4_mul(proj, view)
		};

		clear_values[0] = (VkClearValue) {
			.color.float32 = {0.26f * flash, 0.16f * flash, 0.45f * flash, 1.0f}
		};

		/* update GPU data */
		vk_update_buffer(vk, vk->global_uniform_buffer, &uniforms, 0, sizeof(uniforms));
		
		/* record commands */
		vkCmdBeginRenderPass(cmdbuf, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

        struct Mesh *mesh = &vk->meshes[r->mesh_idx];
        
        VkBuffer buffers[] = { mesh->vert_buf.handle };
		VkDeviceSize offsets[] = { 0 };
		vkCmdBindVertexBuffers(cmdbuf, 0, countof(buffers), buffers, offsets);
        vkCmdBindIndexBuffer(cmdbuf, mesh->index_buf.handle, 0, VK_INDEX_TYPE_UINT16);

		if(r->unlit_shader) {
			vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->flat_pipeline);
		}
		else {
			vkCmdBindPipeline(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->lit_pipeline);
		}

		vkCmdBindDescriptorSets(cmdbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->simple_piepline_layout, 0, 1, &vk->global_desc, 0, NULL);

		vkCmdPushConstants(cmdbuf, vk->simple_piepline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants), &constants);
        vkCmdDrawIndexed(cmdbuf, mesh->index_count, 1, 0, 0, 0);

		vkCmdEndRenderPass(cmdbuf);
	}

	VK_CHECK(vkEndCommandBuffer(cmdbuf));

	/* submission */
	VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

	/* SYNC: The GPU waits on the present semaphore that signals when we
	 * have an available image to draw into.
	 * Then the GPU will signal the render semaphore once it's done executing this cmd buffer.
	 */
	VkSubmitInfo submit_info = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pWaitDstStageMask = &wait_stage,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &vk->present_semaphore,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &vk->render_semaphore,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmdbuf
	};

	/* SYNC: The render fence will be signalled once all commands are executed.
	 * This is what we wait for at the beginning of this function.
	 */
	VK_CHECK(vkQueueSubmit(vk->queue_graphics, 1, &submit_info, vk->render_fence));

	/* SYNC: Here the GPU will wait on the semaphore from the above queue submission before presenting */
	VkPresentInfoKHR present_info = {
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.pSwapchains = &vk->swapchain,
		.swapchainCount = 1,
		.pWaitSemaphores = &vk->render_semaphore,
		.waitSemaphoreCount = 1,
		.pImageIndices = &swapchain_index
	};

	VK_CHECK(vkQueuePresentKHR(vk->queue_graphics, &present_info));
}

int main(int argc, char **argv)
{
	SDL_Init(SDL_INIT_VIDEO);

	s_window = SDL_CreateWindow("vk_meshview",
							    SDL_WINDOWPOS_UNDEFINED,
							    SDL_WINDOWPOS_UNDEFINED,
							    WIDTH, HEIGHT, SDL_WINDOW_VULKAN);

	struct VK *vk = &s_vk;
	struct Render_State *r = &s_render_state;

	vk_init(vk);
	scene_init(r, vk);

	bool running = true;
	while(running) {
		SDL_Event event;
		while(SDL_PollEvent(&event)) {
			if(event.type == SDL_QUIT) {
				running = false;
			}
			else if(event.type == SDL_KEYDOWN) {
				switch(event.key.keysym.sym) {
				case SDLK_SPACE:
					r->unlit_shader = !r->unlit_shader;
					break;
				case SDLK_LEFT:
					r->mesh_idx = (r->mesh_idx - 1) >= 0 ? (r->mesh_idx - 1) : (vk->mesh_count - 1);
					break;
				case SDLK_RIGHT:
					r->mesh_idx = (r->mesh_idx + 1) % vk->mesh_count;
					break;
				}
			}
		}

        // NOTE: Stop doing anything if minimized, because acquiring next image in swapchain will fail.
        uint32_t window_flags = SDL_GetWindowFlags(s_window);
        while(window_flags & SDL_WINDOW_MINIMIZED) {
            SDL_WaitEvent(NULL);
            window_flags = SDL_GetWindowFlags(s_window);
        }

		render(r, vk);

		++s_render_state.frame_number;
	}

	vk_destroy(vk);
	SDL_DestroyWindow(s_window);
	SDL_Quit();

	return 0;
}
