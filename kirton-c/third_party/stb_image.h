/* stb_image - v2.28 - public domain image loader - http://nothings.org/stb_image.h
 *
 * To use, do this in *one* C/C++ file:
 *    #define STB_IMAGE_IMPLEMENTATION
 *    #include "stb_image.h"
 *
 * Quick notes:
 *    Primarily of interest to game developers and other people who can
 *    avoid problematic images and only need the trivial interface
 *    stbi_load(). Supported formats: JPEG, PNG, TGA, BMP, PSD, GIF, HDR,
 *    PIC, PNM (PPM and PGM).
 *
 *    No SIMD or threading. Some formats support only subsets or have
 *    limitations; see original upstream documentation for details.
 *
 * This is a slightly trimmed copy of stb_image.h 2.28 for Kirton runtime.
 */

#ifndef STB_IMAGE_H
#define STB_IMAGE_H

#ifdef __cplusplus
extern "C" {
#endif

// Primary API
unsigned char *stbi_load(char const *filename, int *x, int *y, int *comp, int req_comp);
unsigned char *stbi_load_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp);
void           stbi_image_free(void *retval_from_stbi_load);

// Info
int stbi_info(char const *filename, int *x, int *y, int *comp);
int stbi_info_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp);

// hdr
float *stbi_loadf(char const *filename, int *x, int *y, int *comp, int req_comp);
float *stbi_loadf_from_memory(const unsigned char *buffer, int len, int *x, int *y, int *comp, int req_comp);

// error
const char *stbi_failure_reason(void);

// 8-bit conversions
typedef unsigned char stbi_uc;

// Configuration switches (trimmed)
#ifdef STBI_NO_STDIO
#define STBI_NO_STDIO
#endif

#define STBI_default   0
#define STBI_grey      1
#define STBI_grey_alpha 2
#define STBI_rgb       3
#define STBI_rgb_alpha 4

#ifdef __cplusplus
}
#endif

#endif // STB_IMAGE_H
