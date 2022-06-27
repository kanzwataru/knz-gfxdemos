# knz-gfxdemos
A heavily WIP collection of simple graphics sample programs.
Currently mostly Vulkan, may add some OpenGL scenes as well.

Check out the simple Blender exporter under vk_meshview/tools!

**DISCLAIMER:** This is not meant to be an example on how to correctly use Vulkan. These examples are being made as I learn the Vulkan API, so they may contain mistakes.

## Sample Programs
* vk_hello: Just a simple Hello World for Vulkan
* vk_meshview: Simply displaying arbitrary meshes exported from Blender. Includes Blender exporter and sample data.
* vk_hlsl: Sample using HLSL instead of GLSL, forked from vk_meshview

## How to compile
This project uses a simple CMake setup. It handles copying sample data and compiling shaders as well.

### Install dependencies
You'll need to install CMake and the LunarG SDK.
All other libraries are included within the repo.
The shader compiler ```glslc``` is included within LunarG SDK.

### Build with CMake
Either use the CMake GUI (on Windows) or run CMake from the command line (on Linux):

```
$ mkdir build
$ cd build
$ cmake ..
$ make
```

Each sample program is in its own folder, as it's own target.

## Dependencies
* CGLM (vendored)
* LunarG SDK
* CMake
