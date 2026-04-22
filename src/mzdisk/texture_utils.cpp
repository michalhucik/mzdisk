/**
 * @file texture_utils.cpp
 * @brief Implementace načítání PNG do OpenGL textur.
 *
 * Definuje STB_IMAGE_IMPLEMENTATION pro stb_image.h (IGFD ho nedefinuje
 * bez USE_THUMBNAILS). Používá OpenGL 3.3 core profile API.
 *
 * @par Licence:
 * GNU General Public License v3 (GPLv3)
 */

#define STB_IMAGE_IMPLEMENTATION
#include "libs/igfd/stb/stb_image.h"

#include <SDL3/SDL_opengl.h>
#include <cstdio>

extern "C" {
#include "texture_utils.h"
}


mzdisk_texture_t mzdisk_texture_load_png ( const char *filepath, int *out_width, int *out_height )
{
    int w = 0, h = 0, channels = 0;
    unsigned char *data = stbi_load ( filepath, &w, &h, &channels, 4 );
    if ( !data ) {
        fprintf ( stderr, "Warning: Cannot load image '%s': %s\n", filepath, stbi_failure_reason () );
        return MZDISK_TEXTURE_INVALID;
    }

    GLuint tex = 0;
    glGenTextures ( 1, &tex );
    glBindTexture ( GL_TEXTURE_2D, tex );

    /* LINEAR filtrování - plynulé škálování při různých velikostech UI
       (vlajky v settings, loga, tooltip ikony). Audit L-17 sjednocení
       dokumentace s implementací. */
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
    glTexParameteri ( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

    glTexImage2D ( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data );

    stbi_image_free ( data );

    if ( out_width ) *out_width = w;
    if ( out_height ) *out_height = h;

    return (mzdisk_texture_t) tex;
}


void mzdisk_texture_free ( mzdisk_texture_t tex )
{
    if ( tex != MZDISK_TEXTURE_INVALID ) {
        GLuint gl_tex = (GLuint) tex;
        glDeleteTextures ( 1, &gl_tex );
    }
}
