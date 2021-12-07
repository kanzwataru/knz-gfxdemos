# knz-gfxdemos
A heavily WIP collection of simple graphics sample programs.
Currently mostly Vulkan, may add some OpenGL scenes as well.

Check out the simple Blender exporter under vk_meshview/tools!

### Sample Programs
* vk_hello: Just a simple Hello World for Vulkan
* vk_meshview: Simply displaying arbitrary meshes exported from Blender. Includes Blender exporter and sample data.

### How to compile
This project uses a simple CMake setup. It handles copying sample data and compiling shaders as well.
Either use the CMake GUI (on Windows) or run CMake from the command line (on Linux):

```
$ mkdir build
$ cd build
$ cmake ..
$ make
```

Each sample program is in its own folder.

### Dependencies (a.k.a. Liabilities)
* CGLM
* Vulkan (LunarG SDK)
* glslc
* CMake
