#include "imgui_image.h"

#include <string>
#include <GLES3/gl3.h>



STBIDEF stbi_uc *stbi_load_gif(char const *filename, int **delays, int *width, int *height, int *frames, int *nrChannels, int req_comp);


TextureInfo createTexture(char *ImagePath) {
    int w, h, n;
    static TextureInfo textureInfo_;
    memset(&textureInfo_, 0, sizeof(TextureInfo));    
    stbi_uc *data = stbi_load(ImagePath, &w, &h, &n, 0);
    GLuint texture;
    glGenTextures(1, &texture);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (n == 3) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    }
    stbi_image_free((void *)data);
    textureInfo_.textureId = (ImTextureID)((uintptr_t)texture);
    textureInfo_.w = w;
    textureInfo_.h = h;
    return textureInfo_;
}
TextureInfo createTexture_ALL_FromMem(const unsigned char *buf, int bufSize) {
    int w, h, n;
    static TextureInfo textureInfo_;
    memset(&textureInfo_, 0, sizeof(TextureInfo));    
    stbi_uc *data = stbi_load_from_memory(buf, bufSize, &w, &h, &n, 0);
    GLuint texture;
    glGenTextures(1, &texture);
    glEnable(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (n == 3) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
    }
    stbi_image_free((void *)data);
    textureInfo_.textureId = (ImTextureID)((uintptr_t)texture);
    textureInfo_.w = w;
    textureInfo_.h = h;
    return textureInfo_;
}

TextureInfo_GIF createTexture_gif(char *ImagePath) {
    int w, h, n;
    TextureInfo_GIF textureInfo_gif;
    int *delays = NULL, frames = 100;
    delays = (int *)malloc(sizeof(int) * frames);
    if (delays) {  
        delays[0] = 0;
    }    
    stbi_uc *data2 = stbi_load_gif(ImagePath, &delays, &w, &h, &frames, &n, 0);    
    textureInfo_gif.textureId = (ImTextureID *)malloc(sizeof(ImTextureID) * frames);
    
    for (int II = 0; II < frames; II++) {
        stbi_uc *data = (stbi_uc *)(data2 + (II * w * h * n));
        GLuint texture;
        glGenTextures(1, &texture);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (n == 3) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        }
        if (texture != 0) {
            textureInfo_gif.textureId[II] = (ImTextureID)((uintptr_t)texture);
        }
    }  
    
    stbi_image_free((void *)data2);        
    textureInfo_gif.frames = frames;
    textureInfo_gif.delays = delays;
    textureInfo_gif.w = w;
    textureInfo_gif.h = h;      
    return textureInfo_gif;
}
TextureInfo_GIF createTexture_gif_FromMem(const unsigned char *buf, int bufSize) {
    int w, h, n;
    
    TextureInfo_GIF textureInfo_gif;
    int *delays = NULL, frames = 100;
    delays = (int *)malloc(sizeof(int) * frames);
    if (delays) {  
        delays[0] = 0;
    }    
    stbi_uc *data2 = stbi_load_gif_from_memory(buf, bufSize, &delays, &w, &h, &frames, &n, 0);
    textureInfo_gif.textureId = (ImTextureID *)malloc(sizeof(ImTextureID) * frames);
    
    for (int II = 0; II < frames; II++) {
        stbi_uc *data = (stbi_uc *)(data2 + (II * w * h * n));
        GLuint texture;
        glGenTextures(1, &texture);
        glEnable(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (n == 3) {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, data);
        } else {
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        }
        if (texture != 0) {
            textureInfo_gif.textureId[II] = (ImTextureID)((uintptr_t)texture);
        }
    }  
    
    stbi_image_free((void *)data2);        
    textureInfo_gif.frames = frames;
    textureInfo_gif.delays = delays;
    textureInfo_gif.w = w;
    textureInfo_gif.h = h;      
    return textureInfo_gif;
}

STBIDEF stbi_uc *stbi_load_gif(char const *filename, int **delays, int *width, int *height, int *frames, int *nrChannels, int req_comp) {
    FILE *f = fopen(filename, "rb");
    stbi_uc *result;
    if (!f) return (stbi_uc *)("can't fopen");
    fseek(f, 0, SEEK_END);
    int bufSize = ftell(f);
    stbi_uc *buf = (unsigned char *)malloc(sizeof(unsigned char) * bufSize);
    if (buf) {
        fseek(f, 0, SEEK_SET);
        fread(buf, 1, bufSize, f);
        result = stbi_load_gif_from_memory(buf, bufSize, delays, width, height, frames, nrChannels, req_comp);
        free(buf);
        buf = NULL;
    } else {
        result = (stbi_uc *)("outofmem");
    }
    fclose(f);
    return result;
}

