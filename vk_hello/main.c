#include <vulkan/vulkan.h>
#include <SDL.h>

#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#define WIDTH 1280
#define HEIGHT 720

int main(int argc, char **argv)
{
	SDL_Init(SDL_INIT_VIDEO);

	SDL_Window *window = SDL_CreateWindow("vk_hello",
										  SDL_WINDOWPOS_UNDEFINED,
										  SDL_WINDOWPOS_UNDEFINED,
										  WIDTH, HEIGHT, SDL_WINDOW_VULKAN);

	bool running = true;
	while(running) {
		SDL_Event event;
		while(SDL_PollEvent(&event)) {
			if(event.type == SDL_QUIT) {
				running = false;
			}
		}
	}

	SDL_DestroyWindow(window);
	SDL_Quit();

	return 0;
}
