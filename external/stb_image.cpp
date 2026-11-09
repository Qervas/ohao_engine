// The one stb_image implementation: the asset loaders (ohao_asset) and the texture manager (ohao_gpu_vulkan) both
// decode images, and neither may reach the other's library.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
