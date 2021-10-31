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

#define VK_CHECK(x) \
	do {\
		VkResult err = x;\
		if(err != VK_SUCCESS) {\
			char buf[128];\
			snprintf(buf, sizeof(buf), "Vulkan error %d at line %d\n", err, __LINE__);\
			SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Vulkan Error", buf, NULL);\
			abort();\
		}\
	} while(0);

struct VK {
	VkInstance instance;
};

static struct VK s_vk;
static SDL_Window *s_window;

static void vk_init(struct VK *vk)
{
	const char *extension_names[256];
	unsigned int extension_count;

	SDL_Vulkan_GetInstanceExtensions(s_window, &extension_count, extension_names);

	VkInstanceCreateInfo instance_create_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		//.enabledLayerCount = extension_count,
		//.ppEnabledLayerNames = extension_names
		.enabledExtensionCount = extension_count,
		.ppEnabledExtensionNames = extension_names
	};

	VK_CHECK(vkCreateInstance(&instance_create_info, NULL, &vk->instance));
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
