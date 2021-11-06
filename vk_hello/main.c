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

struct VK {
	/* Instances and Handles */
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;

	/* Presentation */
	VkSurfaceKHR surface;
	VkFormat swapchain_format;
	VkExtent2D swapchain_extent;
	VkSwapchainKHR swapchain;

	VkRenderPass render_pass;
	VkImage swapchain_images[32];
	VkImageView swapchain_image_views[32];
	VkFramebuffer framebuffers[32];
	uint32_t swapchain_image_count;

    /* Pipeline and Shaders */
    VkPipelineLayout pipeline_layout;

	/* Queues and Commands */
	VkQueue queue_graphics;

	VkCommandPool command_pool_graphics;
	VkCommandBuffer command_buffer_graphics;

	uint32_t queue_graphics_idx;

	/* Synchronization */
	VkSemaphore present_semaphore;
	VkSemaphore render_semaphore;
	VkFence render_fence;
};

struct Render_State {
	uint64_t frame_number;
};

static struct VK s_vk;
static struct Render_State s_render_state;
static SDL_Window *s_window;

#define countof(x) (sizeof(x) / sizeof(x[0]))

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
	return module;
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

		// TODO: Favour discrete GPU over integrated
		vk->physical_device = devices[0];
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
		VkPhysicalDeviceFeatures device_features = {0};

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

		// NOTE: There is a warning on Intel GPUs that suggests this function does actually want to be called twice
		vk->swapchain_image_count = image_count;
		VK_CHECK(vkGetSwapchainImagesKHR(vk->device, vk->swapchain, &vk->swapchain_image_count, vk->swapchain_images));

		vk->swapchain_format = format.format;
		vk->swapchain_extent = extent;
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
		}
	}

	/* commands */
	{
		VkCommandPoolCreateInfo pool_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
			.queueFamilyIndex = vk->queue_graphics_idx,
			.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT // allow for resetting individual command buffers, but do we need it?
		};

		VK_CHECK(vkCreateCommandPool(vk->device, &pool_info, NULL, &vk->command_pool_graphics));

		VkCommandBufferAllocateInfo command_alloc_info = {
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
			.commandPool = vk->command_pool_graphics,
			.commandBufferCount = 1,
			.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY
		};

		VK_CHECK(vkAllocateCommandBuffers(vk->device, &command_alloc_info, &vk->command_buffer_graphics));
	}

	/* shaders and pipeline layout */
	{
        VkShaderModule shader_vert = vk_create_shader_module_from_file(vk, "shaders/flat_vert.spv");
        VkShaderModule shader_frag = vk_create_shader_module_from_file(vk, "shaders/flat_frag.spv");

		VkPipelineShaderStageCreateInfo vert_shader_stage_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = shader_vert,
			.pName = "main"
		};

		VkPipelineShaderStageCreateInfo frag_shader_stage_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = shader_frag,
			.pName = "main"
		};

		VkPipelineVertexInputStateCreateInfo vertex_input_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			// Fill these in later
			.vertexBindingDescriptionCount = 0,
			.pVertexBindingDescriptions = NULL,
			.vertexAttributeDescriptionCount = 0,
			.pVertexAttributeDescriptions = NULL,
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
            .cullMode = VK_CULL_MODE_BACK_BIT,
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
        };

        VkPipelineColorBlendAttachmentState color_blend_attachment = {
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
            .blendEnable = VK_FALSE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .alphaBlendOp = VK_BLEND_OP_ADD
        };

        VkPipelineColorBlendStateCreateInfo color_blending = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = VK_FALSE,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = 1,
            .pAttachments = &color_blend_attachment,
            .blendConstants = {0, 0, 0, 0}
        };

        VkDynamicState dynamic_states[] = {
            VK_DYNAMIC_STATE_VIEWPORT
        };

        VkPipelineDynamicStateCreateInfo dynamic_state_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = countof(dynamic_states),
            .pDynamicStates = dynamic_states
        };

        VkPipelineLayoutCreateInfo pipeline_layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 0,
            .pSetLayouts = NULL,
            .pushConstantRangeCount = 0,
            .pPushConstantRanges = NULL
        };

        VK_CHECK(vkCreatePipelineLayout(vk->device, &pipeline_layout_info, NULL, &vk->pipeline_layout));
        vkDestroyShaderModule(vk->device, shader_frag, NULL);
        vkDestroyShaderModule(vk->device, shader_vert, NULL);
	}

    /* render pass */
    {
        VkAttachmentDescription color_attachment = {
            .format = vk->swapchain_format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
        };

        VkAttachmentReference color_attachment_ref = {
            .attachment = 0, // References the pAttachments array in the parent renderpass
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
        };

        VkSubpassDescription subpass = {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments = &color_attachment_ref
        };

        VkRenderPassCreateInfo render_pass_info = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &color_attachment, // This is where VkAttachmentReference::attachment indexes into
            .subpassCount = 1,
            .pSubpasses = &subpass
        };

        VK_CHECK(vkCreateRenderPass(vk->device, &render_pass_info, NULL, &vk->render_pass));
    }

    /* framebuffers */
    {
        VkFramebufferCreateInfo framebuffer_info = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = vk->render_pass,
            .attachmentCount = 1,
            .width = vk->swapchain_extent.width,
            .height = vk->swapchain_extent.height,
            .layers = 1
        };

        for(uint32_t i = 0; i < vk->swapchain_image_count; ++i) {
            framebuffer_info.pAttachments = &vk->swapchain_image_views[i];
            VK_CHECK(vkCreateFramebuffer(vk->device, &framebuffer_info, NULL, &vk->framebuffers[i]));
        }
    }

    /* synchronization */
    {
        VkFenceCreateInfo fence_info = {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };

        VK_CHECK(vkCreateFence(vk->device, &fence_info, NULL, &vk->render_fence));

        VkSemaphoreCreateInfo semaphore_info = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .flags = 0
        };

        VK_CHECK(vkCreateSemaphore(vk->device, &semaphore_info, NULL, &vk->present_semaphore));
        VK_CHECK(vkCreateSemaphore(vk->device, &semaphore_info, NULL, &vk->render_semaphore));
    }
}

static void vk_destroy(struct VK *vk)
{
    vkDeviceWaitIdle(vk->device);

    vkDestroySemaphore(vk->device, vk->render_semaphore, NULL);
    vkDestroySemaphore(vk->device, vk->present_semaphore, NULL);
    vkDestroyFence(vk->device, vk->render_fence, NULL);

    vkDestroyRenderPass(vk->device, vk->render_pass, NULL);

    vkDestroyPipelineLayout(vk->device, vk->pipeline_layout, NULL);

    vkDestroyCommandPool(vk->device, vk->command_pool_graphics, NULL);

	for(uint32_t i = 0; i < vk->swapchain_image_count; ++i) {
        vkDestroyFramebuffer(vk->device, vk->framebuffers[i], NULL);
		vkDestroyImageView(vk->device, vk->swapchain_image_views[i], NULL);
	}

	vkDestroySwapchainKHR(vk->device, vk->swapchain, NULL);
	vkDestroySurfaceKHR(vk->instance, vk->surface, NULL);
	vkDestroyDevice(vk->device, NULL);
	vkDestroyInstance(vk->instance, NULL);
}

static void render(struct Render_State *r, struct VK *vk)
{
	/* sync */
	VK_CHECK(vkWaitForFences(vk->device, 1, &vk->render_fence, true, TIMEOUT));
	VK_CHECK(vkResetFences(vk->device, 1, &vk->render_fence));

	/* SYNC: Here we pass in a semaphore that will be signalled once we have an
	 * image available to draw into */
	uint32_t swapchain_index;
    VK_CHECK(vkAcquireNextImageKHR(vk->device, vk->swapchain, TIMEOUT, vk->present_semaphore, NULL, &swapchain_index));

	/* commands */
	VK_CHECK(vkResetCommandBuffer(vk->command_buffer_graphics, 0));

	VkCommandBuffer cmdbuf = vk->command_buffer_graphics;

	VkCommandBufferBeginInfo cmdbuf_begin_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.pInheritanceInfo = NULL,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
	};

	VK_CHECK(vkBeginCommandBuffer(cmdbuf, &cmdbuf_begin_info));

	/* app-specific */
	{
		const float flash = fabs(sinf(r->frame_number / 120.0f));

		VkClearValue clear_value = {
			.color.float32 = {0.65f * flash, 0.25f * flash, 0.15f * flash, 1.0f}
		};

		VkRenderPassBeginInfo render_pass_info = {
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = vk->render_pass,
			.renderArea = {
				.offset = {},
				.extent = vk->swapchain_extent
			},
			.framebuffer = vk->framebuffers[swapchain_index],
			.clearValueCount = 1,
			.pClearValues = &clear_value
		};

		vkCmdBeginRenderPass(cmdbuf, &render_pass_info, VK_SUBPASS_CONTENTS_INLINE);

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

	s_window = SDL_CreateWindow("vk_hello",
							  SDL_WINDOWPOS_UNDEFINED,
							  SDL_WINDOWPOS_UNDEFINED,
							  WIDTH, HEIGHT, SDL_WINDOW_VULKAN);

	vk_init(&s_vk);

	bool running = true;
	while(running) {
		SDL_Event event;
		while(SDL_PollEvent(&event)) {
			if(event.type == SDL_QUIT) {
				running = false;
			}
		}

		render(&s_render_state, &s_vk);

		++s_render_state.frame_number;
	}

	vk_destroy(&s_vk);
	SDL_DestroyWindow(s_window);
	SDL_Quit();

	return 0;
}
