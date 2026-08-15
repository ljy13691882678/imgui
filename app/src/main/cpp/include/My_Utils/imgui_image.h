#ifndef META_SPACE_IMGUI_IMAGE_H
#define META_SPACE_IMGUI_IMAGE_H


#include "imgui.h"
#include "stb_image.h"

#ifdef __cplusplus
extern "C" {
#endif

struct TextureInfo {
    ImTextureID textureId;
    int x;
    int y;
    int w;
    int h;
};

struct TextureInfo_GIF {
    ImTextureID *textureId;
    int *delays;
    int frames;
    int w;
    int h;
};

extern TextureInfo createTexture(char *ImagePath);
extern TextureInfo createTexture_ALL_FromMem(const unsigned char *buf, int bufSize);
extern TextureInfo_GIF createTexture_gif(char *ImagePath);
extern TextureInfo_GIF createTexture_gif_FromMem(const unsigned char *buf, int bufSize);

#ifdef __cplusplus
}
#endif

#endif //META_SPACE_IMGUI_IMAGE_H
