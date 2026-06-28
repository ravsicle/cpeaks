/* stb single-header implementations isolated in their own translation unit
 * so the heavy code is compiled once and kept out of cpeaks.c. */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "vendor/stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"
