cmake_minimum_required(VERSION 3.10)

project(knz-gfxdemos)
find_package(Vulkan REQUIRED)
find_package(SDL2 REQUIRED)

include_directories(${SDL2_INCLUDE_DIRS} ${CMAKE_SOURCE_DIR}/extern_lib/cglm/include)

function(f_add_target TARGET SRC)
	add_executable(${TARGET} ${SRC})
	target_link_libraries(${TARGET} Vulkan::Vulkan ${SDL2_LIBRARIES})
	
	if(WIN32)
		add_custom_command(TARGET ${TARGET} POST_BUILD
			COMMAND ${CMAKE_COMMAND} -E copy_if_different
			"${SDL2_DIR}/lib/x64/SDL2.dll" "${CMAKE_BINARY_DIR}/SDL2.dll")
	else()
		target_link_libraries(${TARGET} m)
	endif()
	
	set_target_properties(${TARGET} PROPERTIES
		RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/${TARGET}"
	)
endfunction()

# Based on https://www.reddit.com/r/vulkan/comments/kbaxlz/what_is_your_workflow_when_compiling_shader_files/
function(f_add_shader TARGET SHADER STAGE)
    find_program(GLSLC glslc)

    #set(current-shader-path ${CMAKE_CURRENT_SOURCE_DIR}/src/shaders/${SHADER})
    #set(current-output-path ${CMAKE_BINARY_DIR}/shaders/${SHADER}.spv)
    set(current-shader-path ${CMAKE_CURRENT_SOURCE_DIR}/${TARGET}/shaders/${SHADER}.glsl)
    set(current-output-path ${CMAKE_BINARY_DIR}/${TARGET}/shaders/${SHADER}.spv)

    # Add a custom command to compile GLSL to SPIR-V.
    get_filename_component(current-output-dir ${current-output-path} DIRECTORY)
    file(MAKE_DIRECTORY ${current-output-dir})

    add_custom_command(
           OUTPUT ${current-output-path}
           COMMAND ${GLSLC} -fshader-stage=${STAGE} -o ${current-output-path} ${current-shader-path}
           DEPENDS ${current-shader-path}
           IMPLICIT_DEPENDS CXX ${current-shader-path}
           VERBATIM)

    # Make sure our build depends on this output.
    set_source_files_properties(${current-output-path} PROPERTIES GENERATED TRUE)
    target_sources(${TARGET} PRIVATE ${current-output-path} ${current-shader-path})
endfunction()

function(f_add_data TARGET FILE)
    set(current-input-path ${CMAKE_CURRENT_SOURCE_DIR}/${TARGET}/data/${FILE})
    set(current-output-path ${CMAKE_BINARY_DIR}/${TARGET}/data/${FILE})

    get_filename_component(current-output-dir ${current-output-path} DIRECTORY)
    file(MAKE_DIRECTORY ${current-output-dir})

    add_custom_command(
        OUTPUT ${current-output-path}
        COMMAND ${CMAKE_COMMAND} ARGS -E copy ${current-input-path} ${current-output-path}
        DEPENDS ${current-input-path}
        IMPLICIT_DEPENDS CXX ${current-input-path}
    )

    set_source_files_properties(${current-output-path} PROPERTIES GENERATED TRUE)
    target_sources(${TARGET} PRIVATE ${current-output-path} ${current-input-path})
endfunction()

# vk_hello
f_add_target(vk_hello vk_hello/main.c)

f_add_shader(vk_hello flat_frag frag)
f_add_shader(vk_hello flat_vert vert)

f_add_shader(vk_hello rgb_frag frag)
f_add_shader(vk_hello rgb_vert vert)

# vk_meshview
f_add_target(vk_meshview vk_meshview/main.c)

f_add_shader(vk_meshview flat_frag frag)
f_add_shader(vk_meshview flat_vert vert)

f_add_shader(vk_meshview lit_frag frag)
f_add_shader(vk_meshview lit_vert vert)

f_add_data(vk_meshview cube.bin)
f_add_data(vk_meshview suzanne.bin)
