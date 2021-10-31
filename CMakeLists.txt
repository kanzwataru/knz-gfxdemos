cmake_minimum_required(VERSION 3.10)

project(vk_hello)
find_package(Vulkan REQUIRED)
find_package(SDL2 REQUIRED)

include_directories(${SDL2_INCLUDE_DIRS})

add_executable(vk_hello vk_hello/main.c)
target_link_libraries(vk_hello Vulkan::Vulkan ${SDL2_LIBRARIES})

if(WIN32)
	add_custom_command(TARGET vk_hello POST_BUILD
		COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${SDL2_DIR}/lib/x64/SDL2.dll" "${CMAKE_BINARY_DIR}/SDL2.dll")
endif()

