#include <vulkan/vulkan.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>

#define WIDTH 1280
#define HEIGHT 720

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

struct VK {
	VkInstance instance;
	VkPhysicalDevice physical_device;
	VkDevice device;

	VkSurfaceKHR surface;
	VkFormat swapchain_format;
	VkExtent2D swapchain_extent;
	VkSwapchainKHR swapchain;
	VkImage swapchain_images[32];
	VkImageView swapchain_image_views[32];
	uint32_t swapchain_image_count;

	VkShaderModule shader_vert;
	VkShaderModule shader_frag;

	VkQueue queue_present;

	uint32_t queue_graphics_idx;
	uint32_t queue_present_idx;
};

static struct VK s_vk;
static SDL_Window *s_window;

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
		bool found_present = false;
		for(uint32_t i = 0; i < queue_families_count; ++i) {
			if(!found_graphics && queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
				found_graphics = true;
				vk->queue_graphics_idx = i;
			}

			VkBool32 present_support = false;
			vkGetPhysicalDeviceSurfaceSupportKHR(vk->physical_device, i, vk->surface, &present_support);
			if(!found_present && present_support) {
				found_present = true;
				vk->queue_present_idx = i;
			}
		}

		CHECK(found_graphics, "No graphics queue found");
		CHECK(found_present, "No present queue found");
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
		VkDeviceQueueCreateInfo queue_infos[2];
		uint32_t queue_indices[2] = {
			vk->queue_graphics_idx,
			vk->queue_present_idx
		};

		uint32_t queue_count = vk->queue_graphics_idx == vk->queue_present_idx ? 1 : 2;

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

		vkGetDeviceQueue(vk->device, vk->queue_present_idx, 0, &vk->queue_present);
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
			.oldSwapchain = VK_NULL_HANDLE
		};

		if(vk->queue_graphics_idx != vk->queue_present_idx) {
			uint32_t queue_family_indices[] = {
				vk->queue_graphics_idx,
				vk->queue_present_idx
			};

			create_info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
			create_info.queueFamilyIndexCount = 2;
			create_info.pQueueFamilyIndices = queue_family_indices;
		}
		else {
			create_info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		}

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

	/* shaders and pipeline layout */
	{
		vk->shader_vert = vk_create_shader_module_from_file(vk, "shaders/flat_vert.spv");
		vk->shader_frag = vk_create_shader_module_from_file(vk, "shaders/flat_frag.spv");

		VkPipelineShaderStageCreateInfo vert_shader_stage_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vk->shader_vert,
			.pName = "main"
		};

		VkPipelineShaderStageCreateInfo frag_shader_stage_info = {
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = vk->shader_frag,
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
	}
}

static void vk_destroy(struct VK *vk)
{
	vkDestroyShaderModule(vk->device, vk->shader_frag, NULL);
	vkDestroyShaderModule(vk->device, vk->shader_vert, NULL);

	for(uint32_t i = 0; i < vk->swapchain_image_count; ++i) {
		vkDestroyImageView(vk->device, vk->swapchain_image_views[i], NULL);
	}

	vkDestroySwapchainKHR(vk->device, vk->swapchain, NULL);
	vkDestroySurfaceKHR(vk->instance, vk->surface, NULL);
	vkDestroyDevice(vk->device, NULL);
	vkDestroyInstance(vk->instance, NULL);
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
	}

	vk_destroy(&s_vk);
	SDL_DestroyWindow(s_window);
	SDL_Quit();

	return 0;
}
