#include <vulkan/vulkan.h>
#include <SDL.h>
#include <SDL_vulkan.h>

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>

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

	uint32_t queue_graphics_idx;
};

static struct VK s_vk;
static SDL_Window *s_window;

#define CHECK(x_, msg_) do { if(!x_) { panic(msg_); } } while(0);
static void panic(const char *message)
{
	fprintf(stderr, "%s\n", message);
	SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Critical Error", message, NULL);
	abort();
}

static void vk_init_instance(struct VK *vk)
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

static void vk_init_physical_device(struct VK *vk)
{
	VkPhysicalDevice devices[256];
	uint32_t device_count = countof(devices);

	vkEnumeratePhysicalDevices(vk->instance, &device_count, devices);
	CHECK(device_count, "No GPUs found");

	// TODO: Favour discrete GPU over integrated
	vk->physical_device = devices[0];
}

static void vk_init_queues(struct VK *vk)
{
	VkQueueFamilyProperties queue_families[32];
	uint32_t queue_families_count = countof(queue_families);
	vkGetPhysicalDeviceQueueFamilyProperties(vk->physical_device, &queue_families_count, queue_families);

	bool found = false;
	for(uint32_t i = 0; i < queue_families_count; ++i) {
		if(queue_families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			found = true;
			vk->queue_graphics_idx = i;
			break;
		}
	}

	CHECK(found, "No graphics queue found");
}

static void vk_init(struct VK *vk)
{
	vk_init_instance(vk);
	vk_init_physical_device(vk);
	vk_init_queues(vk);
}

static void vk_destroy(struct VK *vk)
{
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
