# MVR engine - Modern Vulkan Rendering engine

A rendering engine what targets modern hardware and latest **vulkan** (gpu driven rendering):

- mesh shaders
- raytracing/pathtracing with latest EXT from nvidia
- all bindless, no descriptor sets
  - images and samplers: `VK_EXT_descriptor_heap` (one resource heap + one sampler heap, shaders access them through `VK_KHR_shader_untyped_pointers`)
  - buffers: `VK_KHR_buffer_device_address` only
- slang for shaders
- and of course dlss

**Requirements:**

- Nvidia only + needs beta drivers
- 2000 series or later
- Windows only for now: the Linux build has not been tested
- amd could theoretically work but when raytracing will be done it will not work, also dlss

A proof of idea project bassically and my graduation project at the same time

LLms tools are used to assist research and some things like parsers, build system configuration, debuging errors/sanitazers, comments, while rendering code i write myself most of the time to study computer graphics. Overall all info used is from the vulkan docs and other docs of used libs.
