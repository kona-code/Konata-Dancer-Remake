//  NO_AI POLICY
//  
//  This file is subject to the repository's strict NO_AI policy.
//  See: NO_AI.md
//  
//  AI/ML systems of any kind are prohibited from being used to:
//  
//      - generate, modify, review, analyze, or transform this file;
//      - index, embed, ingest, train on, or otherwise process its contents;
//      - provide automated assistance or contributions to this repository.
//  
//  This prohibition applies to generative AI, machine learning,
//  language models, embeddings, AI coding assistants, AI indexing,
//  automated agents, and substantially similar technologies.
//  
//  
//  No exceptions are implied. Any exception requires prior written
//  authorization from the repository owner.
//  
//  
//  Copyright © 2026 konacode. All rights reserved.

#include "core.h"
#include "konanix.h"
#include "./util/logger.h"
#include <GLFW/glfw3.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <stdlib.h>
#include <thread>
#include <fstream>
#include <chrono>
#include <array>
#include <filesystem>
#include <vector>
#include <stdexcept>
#include <algorithm>

#include <gif_lib.h>

#include "globals/window.h"

// #define STB_ONLY_GIF
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_THREAD_LOCALS
#include "../third_party/stb_image.h"
// #include <ktx.h>

#include <signal.h>

#include "./assets/konata.c" // changing to bin had no effect

// #include <GLFW/glfw3.h>
// #include <GLFW/glfw3native.h>

struct MemoryGifReader {
    const unsigned char *data;
    size_t size;
    size_t pos;
};

std::filesystem::path path;

// giflib helpers
static std::vector<uint8_t> read_binary_file(const std::filesystem::path& p) {
    std::ifstream file(p, std::ios::binary | std::ios::ate);
    if (!file) {
        logger::log("Failed to open \""+p.string()+"\"!",logger::exc);
        throw std::runtime_error("failed to open file: " + p.string());
    }

    std::streamsize size = file.tellg();
    if (size <= 0) {
        logger::log("File \""+p.string()+"\" is empty!",logger::exc);
        throw std::runtime_error("empty file: " + p.string());
    }

    std::vector<uint8_t> data(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        logger::log("Failed to read file \""+p.string()+"\"!",logger::exc);
        throw std::runtime_error("failed to read file: " + p.string());
    }
    return data;
}

static std::array<uint8_t, 4> gif_color_at(const ColorMapObject* cmap, int index) {
    if (!cmap || index < 0 || index >= cmap->ColorCount) return {0, 0, 0, 0};
    const GifColorType& c = cmap->Colors[index];
    return {c.Red, c.Green, c.Blue, 255};
}

static int gif_gce_disposal(const SavedImage& img) {
    for (int i = 0; i < img.ExtensionBlockCount; ++i) {
        const ExtensionBlock& eb = img.ExtensionBlocks[i];
        if (eb.Function == GRAPHICS_EXT_FUNC_CODE && eb.ByteCount >= 4) {
            return (eb.Bytes[0] >> 2) & 0x7;
        }
    }
    return 0;
}

static int gif_gce_delay_ms(const SavedImage& img) {
    for (int i = 0; i < img.ExtensionBlockCount; ++i) {
        const ExtensionBlock& eb = img.ExtensionBlocks[i];
        if (eb.Function == GRAPHICS_EXT_FUNC_CODE && eb.ByteCount >= 4) {
            int hundredths = eb.Bytes[1] | (eb.Bytes[2] << 8);
            return std::max(10, hundredths * 10);
        }
    }
    return 100;
}

static int gif_gce_transparent_index(const SavedImage& img) {
    for (int i = 0; i < img.ExtensionBlockCount; ++i) {
        const ExtensionBlock& eb = img.ExtensionBlocks[i];
        if (eb.Function == GRAPHICS_EXT_FUNC_CODE && eb.ByteCount >= 4) {
            const bool has_transparency = (eb.Bytes[0] & 0x01) != 0;
            return has_transparency ? static_cast<int>(eb.Bytes[3]) : -1;
        }
    }
    return -1;
}

static void fill_canvas(std::vector<uint8_t>& canvas, const std::array<uint8_t, 4>& rgba) {
    for (size_t i = 0; i + 3 < canvas.size(); i += 4) {
        canvas[i + 0] = rgba[0];
        canvas[i + 1] = rgba[1];
        canvas[i + 2] = rgba[2];
        canvas[i + 3] = rgba[3];
    }
}

static void clear_rect_to_bg(std::vector<uint8_t>& canvas,
                             int canvas_w, int canvas_h,
                             int left, int top, int w, int h,
                             const std::array<uint8_t, 4>& bg) {
    for (int y = 0; y < h; ++y) {
        const int dy = top + y;
        if (dy < 0 || dy >= canvas_h) continue;
        for (int x = 0; x < w; ++x) {
            const int dx = left + x;
            if (dx < 0 || dx >= canvas_w) continue;
            const size_t p = static_cast<size_t>(dy * canvas_w + dx) * 4;
            // canvas[p + 0] = bg[0];
            // canvas[p + 1] = bg[1];
            // canvas[p + 2] = bg[2];
            // canvas[p + 3] = bg[3];
            canvas[p + 0] = 0;
            canvas[p + 1] = 0;
            canvas[p + 2] = 0;
            canvas[p + 3] = 0;

        }
    }
}

static void draw_indexed_frame(std::vector<uint8_t>& canvas,
                               int canvas_w, int canvas_h,
                               const SavedImage& img,
                               const ColorMapObject* cmap,
                               int transparent_index) {
    const int left = img.ImageDesc.Left;
    const int top  = img.ImageDesc.Top;
    const int w    = img.ImageDesc.Width;
    const int h    = img.ImageDesc.Height;

    const GifByteType* src = img.RasterBits;

    for (int y = 0; y < h; ++y) {
        const int dy = top + y;
        if (dy < 0 || dy >= canvas_h) continue;

        for (int x = 0; x < w; ++x) {
            const int dx = left + x;
            if (dx < 0 || dx >= canvas_w) continue;

            const int idx = src[y * w + x];
            if (idx == transparent_index) continue;

            const auto c = gif_color_at(cmap, idx);
            const size_t p = static_cast<size_t>(dy * canvas_w + dx) * 4;
            canvas[p + 0] = c[0];
            canvas[p + 1] = c[1];
            canvas[p + 2] = c[2];
            canvas[p + 3] = 255;
        }
    }
}

static int read_from_memory(GifFileType *gif, GifByteType *dst, int len) {
    MemoryGifReader *r = (MemoryGifReader *)gif->UserData;
    size_t remaining = r->size - r->pos;

    if (remaining == 0)
        return 0; // eof

    if ((size_t)len > remaining)
        len = (int)remaining;

    memcpy(dst, r->data + r->pos, (size_t)len);
    r->pos += (size_t)len;
    return len;
}

GifFileType *open_gif_from_memory(const unsigned char *gif_bytes,
                                  size_t gif_size,
                                  int *err) {
    MemoryGifReader *reader = static_cast<MemoryGifReader*>(malloc(sizeof(*reader)));
    if (!reader)
        return NULL;

    reader->data = gif_bytes;
    reader->size = gif_size;
    reader->pos = 0;

    GifFileType *gif = DGifOpen(reader, read_from_memory, err);
    if (!gif) {
        free(reader);
        return NULL;
    }

    gif->UserData = reader;
    return gif;
}

void close_gif_from_memory(GifFileType *gif) {
    if (!gif) return;
    free(gif->UserData);
    DGifCloseFile(gif, NULL);
}

// main GIF loader
static GifAnimation load_gif_animation(const std::filesystem::path& path) {
    int err = 0;
    GifFileType* gif;

    if (!path.empty()) gif = DGifOpenFileName(path.string().c_str(), &err);
    else gif = open_gif_from_memory(konata_bytecode, konata_bytecode_len, &err);

    if (!gif) {
        logger::log("DGifOpenFileName failed for \""+path.string()+"\"! Exception details: "+std::to_string(err),logger::exc);
        throw std::runtime_error("DGifOpenFileName failed for: " + path.string() + " err=" + std::to_string(err));
    }

    if (DGifSlurp(gif) == GIF_ERROR) {
        int close_err = 0;
        DGifCloseFile(gif, &close_err);
        logger::log("DGifSlurp failed for \""+path.string()+"\"!",logger::exc);
        throw std::runtime_error("DGifSlurp failed for: " + path.string());
    }

    GifAnimation anim;
    anim.width = gif->SWidth;
    anim.height = gif->SHeight;
    anim.frames.reserve(std::max(0, gif->ImageCount));

    std::vector<uint8_t> canvas(static_cast<size_t>(anim.width) * anim.height * 4, 0);

    // std::array<uint8_t, 4> bg = {0, 0, 0, 0};
    // if (gif->SColorMap &&
    //     gif->SBackGroundColor >= 0 &&
    //     gif->SBackGroundColor < gif->SColorMap->ColorCount) {
    //     bg = gif_color_at(gif->SColorMap, gif->SBackGroundColor);
    // }
    fill_canvas(canvas, {0,0,0,0});

    for (int i = 0; i < gif->ImageCount; ++i) {
        const SavedImage& img = gif->SavedImages[i];
        const ColorMapObject* cmap = img.ImageDesc.ColorMap ? img.ImageDesc.ColorMap : gif->SColorMap;
        if (!cmap) {
            int close_err = 0;
            DGifCloseFile(gif, &close_err);
            logger::log("GIF frame (i="+std::to_string(i)+") has no color map!",logger::exc);
            throw std::runtime_error("GIF frame has no color map");
        }

        const int disposal = gif_gce_disposal(img);
        const int delay_ms = gif_gce_delay_ms(img);
        const int transparent_index = gif_gce_transparent_index(img);

        std::vector<uint8_t> before = canvas; // for disposal 3
        draw_indexed_frame(canvas, anim.width, anim.height, img, cmap, transparent_index);

        GifFrame frame;
        frame.delay_ms = delay_ms;
        frame.rgba = canvas;
        anim.frames.push_back(std::move(frame));

        if (disposal == 2) {
            clear_rect_to_bg(canvas,
                             anim.width, anim.height,
                             img.ImageDesc.Left, img.ImageDesc.Top,
                             img.ImageDesc.Width, img.ImageDesc.Height,
                             {0,0,0,0});
        } else if (disposal == 3) {
            canvas.swap(before);
        }
    }

    int close_err = 0;
    DGifCloseFile(gif, &close_err);
    return anim;
}

void terminate_handler(int s) {
    printf("\n");
    logger::log("Caught signal "+std::to_string(s)+"! Terminating...");
    konanix::cleanup();
    logger::log("Terminated successfully!");
    exit(0);
}

int main(int argc, char *argv[]) {

    printf(
        "[0m[38;2;164;148;237mxxxxxxxxxxx[38;2;164;148;237mx[38;2;164;148;237mx[38;2;158;139;214md[38;2;150;124;168mo[38;2;147;119;153ml[38;2;146;122;166mo[38;2;152;134;208md[38;2;164;148;237mx[38;2;164;148;237mx[38;2;164;148;237mxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx[0m\n"
        "[0m[38;2;164;148;237mxxxxxxxxxx[38;2;164;148;237mx[38;2;151;131;190mo[38;2;148;114;117ml[38;2;141;98;72m:[38;2;141;95;68m:[38;2;142;95;69m:[38;2;138;92;65m:[38;2;135;91;67m:[38;2;134;97;93m:[38;2;140;113;144ml[38;2;153;136;212md[38;2;164;148;237mx[38;2;164;148;237mxxxxxxxxxxxxxxxxxxxxxxxxxxxx[0m\n"
        "[0m[38;2;164;148;237mxxxxxxxxx[38;2;162;146;232mx[38;2;143;117;143ml[38;2;140;99;73m:[38;2;154;104;77mc[38;2;205;142;116mx[38;2;226;158;132mk[38;2;231;162;136mk[38;2;222;156;130mk[38;2;191;133;104md[38;2;158;107;80mc[38;2;138;92;66m:[38;2;132;92;71m:[38;2;136;111;146ml[38;2;162;146;233mx[38;2;164;148;237mxxxxxxxxxxxxxxxxxxxxxxxxxxx[0m\n"
        "[0m[38;2;164;148;237mxxxxxxxx[38;2;164;148;237mx[38;2;143;118;143ml[38;2;163;121;89ml[38;2;133;90;62m:[38;2;203;141;113mx[38;2;231;161;135mk[38;2;231;161;135mk[38;2;227;159;133mk[38;2;211;148;119mx[38;2;211;148;117mx[38;2;210;148;118mx[38;2;166;114;90ml[38;2;130;87;62m;[38;2;130;90;65m:[38;2;123;99;125m:[38;2;147;124;171mo[38;2;146;121;162ml[38;2;141;118;157ml[38;2;138;116;151ml[38;2;146;123;155mo[38;2;146;125;170mo[38;2;149;130;195mo[38;2;160;144;229mx[38;2;164;148;237mx[38;2;164;148;237mx[38;2;164;148;237mxxxx[38;2;164;148;237mx[38;2;162;147;235mx[38;2;159;145;232mx[38;2;163;148;235mx[38;2;164;148;237mx[38;2;164;148;237mx[38;2;164;148;237mxxxxxxx[0m\n"
        "[0m[38;2;164;148;237mxxxxxxxx[38;2;154;137;213md[38;2;157;118;91ml[38;2;163;121;88ml[38;2;139;96;68m:[38;2;197;135;108md[38;2;222;156;127mk[38;2;216;150;122mx[38;2;194;133;107md[38;2;172;118;93ml[38;2;158;108;81mc[38;2;141;98;71m:[38;2;125;119;74mc[38;2;111;125;74mc[38;2;134;190;101mx[38;2;178;212;161m0[38;2;121;147;86ml[38;2;137;118;86mc[38;2;141;108;88mc[38;2;134;103;85mc[38;2;137;106;87mc[38;2;149;116;97ml[38;2;148;116;96ml[38;2;136;104;91mc[38;2;137;109;121mc[38;2;142;120;160ml[38;2;151;134;206md[38;2;164;148;236mx[38;2;164;148;237mx[38;2;158;144;231mx[38;2;114;133;205mo[38;2;87;131;196ml[38;2;83;130;194ml[38;2;82;123;184mc[38;2;88;123;184mc[38;2;100;127;191ml[38;2;122;138;218mo[38;2;156;147;235mx[38;2;164;148;237mx[38;2;164;148;237mxxxx[0m\n"
        "[0m[38;2;164;148;237mxxxxxxxx[38;2;150;132;194mo[38;2;165;125;94mo[38;2;162;122;91ml[38;2;160;119;88ml[38;2;162;111;85ml[38;2;190;132;107md[38;2;155;105;80mc[38;2;137;92;65m:[38;2;145;101;70mc[38;2;131;110;71mc[38;2;128;175;93md[38;2;101;141;76mc[38;2;119;168;91mo[38;2;121;161;99mo[38;2;132;169;105md[38;2;127;170;85md[38;2;118;118;75mc[38;2;153;118;98ml[38;2;142;110;91mc[38;2;129;100;83m:[38;2;124;95;80m:[38;2;149;120;101ml[38;2;156;129;112mo[38;2;160;133;117mo[38;2;159;132;116mo[38;2;156;130;116mo[38;2;151;133;136mo[38;2;166;149;169md[38;2;146;142;161md[38;2;122;137;161mo[38;2;101;113;143mc[38;2;90;106;142m:[38;2;111;120;180ml[38;2;129;135;209mo[38;2;123;133;204mo[38;2;85;141;224mo[38;2;97;149;239mo[38;2;158;146;233mx[38;2;164;148;237mxxxx[0m\n"
        "[0m[38;2;164;148;237mxxxxxxxx[38;2;146;130;203mo[38;2;156;118;90ml[38;2;162;123;92ml[38;2;161;122;93ml[38;2;155;113;84ml[38;2;128;85;60m;[38;2;139;95;67m:[38;2;147;104;73mc[38;2;151;109;81mc[38;2;139;129;92ml[38;2;151;200;121mk[38;2;137;193;105mx[38;2;133;178;98md[38;2;122;162;81mo[38;2;120;143;77ml[38;2;128;116;82mc[38;2;151;117;97ml[38;2;151;119;98ml[38;2;151;118;99ml[38;2;150;118;99ml[38;2;137;107;90mc[38;2;142;112;93mc[38;2;149;124;107ml[38;2;166;140;124md[38;2;178;150;134md[38;2;178;150;133md[38;2;171;142;123md[38;2;173;145;126md[38;2;175;147;128md[38;2;174;144;126md[38;2;170;140;121md[38;2;138;108;90mc[38;2;91;72;65m,[38;2;152;137;216md[38;2;117;142;226mo[38;2;92;157;248md[38;2;92;150;236mo[38;2;151;142;227md[38;2;164;148;237mxxxx[0m\n"
        "[0m[38;2;164;148;237mxxxxxxxx[38;2;163;147;233mx[38;2;145;112;102mc[38;2;164;125;97mo[38;2;160;120;94ml[38;2;156;114;87ml[38;2;147;103;73mc[38;2;123;84;60m;[38;2;148;108;80mc[38;2;156;120;95ml[38;2;155;120;96ml[38;2;135;108;83mc[38;2;139;135;91ml[38;2;140;127;90ml[38;2;141;114;90mc[38;2;152;118;98ml[38;2;152;118;98ml[38;2;151;118;99ml[38;2;149;118;99ml[38;2;145;114;96mc[38;2;127;112;111mc[38;2;107;116;142mc[38;2;85;115;161mc[38;2;78;123;183mc[38;2;80;124;186mc[38;2;78;113;162m:[38;2;89;109;143m:[38;2;113;112;126mc[38;2;126;118;123mc[38;2;127;117;120mc[38;2;132;115;113mc[38;2;144;114;98mc[38;2;140;107;89mc[38;2;144;114;101mc[38;2;101;129;195ml[38;2;92;154;240md[38;2;95;145;224mo[38;2;133;138;217md[38;2;164;148;237mx[38;2;164;148;237mxxxx[0m\n"
        "[0m[38;2;164;148;237mxxxxxxx[38;2;164;148;237mx[38;2;143;123;181mo[38;2;124;90;76m:[38;2;170;131;105mo[38;2;164;126;101mo[38;2;151;109;83mc[38;2;145;102;75mc[38;2;113;78;58m,[38;2;149;114;93ml[38;2;155;119;98ml[38;2;154;119;98ml[38;2;153;119;98ml[38;2;153;119;97ml[38;2;152;119;98ml[38;2;151;118;98ml[38;2;150;118;98ml[38;2;146;115;97ml[38;2;128;113;112mc[38;2;108;121;151mc[38;2;103;144;203mo[38;2;117;168;239mx[38;2;101;132;190ml[38;2;90;130;191ml[38;2;89;154;246md[38;2;89;154;248md[38;2;88;150;235mo[38;2;88;143;220mo[38;2;88;146;226mo[38;2;89;150;235mo[38;2;90;153;240mo[38;2;89;143;214mo[38;2;111;110;123mc[38;2;124;107;103mc[38;2;107;121;150mc[38;2;104;116;142mc[38;2;122;115;128mc[38;2;150;132;182mo[38;2;162;145;231mx[38;2;164;148;237mx[38;2;164;148;237mxxxx[0m\n"
        "[0m[38;2;164;148;237mxxxxxx[38;2;164;148;237mx[38;2;151;131;188mo[38;2;145;106;85mc[38;2;140;97;72m:[38;2;143;103;81mc[38;2;143;105;82mc[38;2;137;96;72m:[38;2;128;90;69m:[38;2;133;100;81m:[38;2;154;119;97ml[38;2;153;119;98ml[38;2;152;118;98ml[38;2;151;118;98ml[38;2;149;116;96ml[38;2;144;108;90mc[38;2;132;111;102mc[38;2;110;115;135mc[38;2;91;127;178ml[38;2;87;136;203ml[38;2;83;133;201ml[38;2;100;161;247md[38;2;130;143;186mo[38;2;220;138;106mx[38;2;124;143;190mo[38;2;105;161;240md[38;2;85;145;232mo[38;2;89;154;246md[38;2;90;154;247md[38;2;89;154;248md[38;2;91;156;247md[38;2;91;156;247md[38;2;87;138;206ml[38;2;81;116;167mc[38;2;92;109;138m:[38;2;131;114;109mc[38;2;141;103;83mc[38;2;156;110;90mc[38;2;149;106;85mc[38;2;135;100;93m:[38;2;151;132;195mo[38;2;164;148;237mx[38;2;164;148;237mxxx[0m\n"
        "[0m[38;2;164;148;237mxxxxx[38;2;164;148;237mx[38;2;151;132;191mo[38;2;151;117;100ml[38;2;149;109;85mc[38;2;148;108;84mc[38;2;156;121;98ml[38;2;148;114;93mc[38;2;138;107;88mc[38;2;119;91;74m;[38;2;128;98;81m:[38;2;149;117;97ml[38;2;150;118;98ml[38;2;147;115;96ml[38;2;131;103;92mc[38;2;109;108;122m:[38;2;93;126;178ml[38;2;81;128;190mc[38;2;84;131;192ml[38;2;102;128;172ml[38;2;150;120;112ml[38;2;83;124;180mc[38;2;96;145;221mo[38;2;227;174;149mO[38;2;238;166;131mO[38;2;120;135;172mo[38;2;91;140;208ml[38;2;98;159;245md[38;2;115;170;247mx[38;2;113;168;246mx[38;2;119;171;246mx[38;2;107;165;247mx[38;2;106;163;245md[38;2;85;128;186ml[38;2;90;136;197ml[38;2;86;126;178mc[38;2;83;116;163mc[38;2;107;116;147mc[38;2;160;143;159md[38;2;210;158;148mk[38;2;192;135;111md[38;2;127;89;80m:[38;2;160;144;228mx[38;2;164;148;237mx[38;2;164;148;237mx[38;2;164;148;237mx[0m\n"
        "[0m[38;2;164;148;237mxxxx[38;2;164;148;237mx[38;2;154;135;196md[38;2;155;121;106ml[38;2;152;117;95ml[38;2;151;116;93ml[38;2;158;123;102ml[38;2;160;126;106mo[38;2;158;125;104ml[38;2;150;118;99ml[38;2;129;100;84m:[38;2;125;98;81m:[38;2;134;105;88mc[38;2;117;107;111mc[38;2;93;114;148mc[38;2;86;135;201ml[38;2;89;150;237mo[38;2;83;136;212ml[38;2;85;120;169mc[38;2;125;116;125mc[38;2;204;151;121mx[38;2;229;178;146mO[38;2;98;127;171ml[38;2;145;144;164md[38;2;239;184;153m0[38;2;226;166;132mO[38;2;105;113;138mc[38;2;84;137;209ml[38;2;87;149;233mo[38;2;93;158;248md[38;2;101;162;247md[38;2;103;163;247md[38;2;122;173;245mx[38;2;122;168;236mx[38;2;85;123;176mc[38;2;103;150;217mo[38;2;112;153;214md[38;2;90;124;175mc[38;2;111;148;207mo[38;2;88;120;166mc[38;2;99;140;202mo[38;2;118;147;207md[38;2;113;126;167ml[38;2;127;138;215mo[38;2;143;144;228md[38;2;157;144;229mx[38;2;164;148;237mx[0m\n"
        "[0m[38;2;164;148;237mxxx[38;2;164;148;237mx[38;2;153;135;201md[38;2;155;124;111ml[38;2;150;117;97ml[38;2;146;111;91mc[38;2;161;127;109mo[38;2;152;120;101ml[38;2;149;118;98ml[38;2;149;118;98ml[38;2;149;118;98ml[38;2;134;105;88mc[38;2;94;93;104m;[38;2;84;116;160mc[38;2;84;133;196ml[38;2;87;147;228mo[38;2;89;154;248md[38;2;87;148;235mo[38;2;116;111;126mc[38;2;89;71;62m,[38;2;100;95;78m;[38;2;75;86;57m,[38;2;91;102;88m;[38;2;116;121;128mc[38;2;120;112;115mc[38;2;184;144;121md[38;2;217;155;123mk[38;2;101;123;162mc[38;2;82;135;204ml[38;2;87;152;238mo[38;2;89;153;242mo[38;2;90;154;244md[38;2;90;152;237mo[38;2;89;143;215mo[38;2;86;133;195ml[38;2;85;123;176mc[38;2;106;156;227md[38;2;128;174;244mk[38;2;100;137;191ml[38;2;115;162;235md[38;2;103;145;210mo[38;2;97;121;164mc[38;2;113;101;107m:[38;2;116;112;157mc[38;2;147;142;223md[38;2;160;146;233mx[38;2;164;148;237mx[38;2;164;148;237mx[0m\n"
        "[0m[38;2;164;148;237mxx[38;2;164;148;237mx[38;2;153;136;205md[38;2;152;121;114ml[38;2;144;111;92mc[38;2;147;115;98ml[38;2;157;125;107ml[38;2;150;119;99ml[38;2;149;119;99ml[38;2;149;119;100ml[38;2;147;118;100ml[38;2;120;101;94m:[38;2;79;102;139m:[38;2;79;120;177mc[38;2;82;132;196ml[38;2;86;150;234mo[38;2;86;154;247mo[38;2;86;155;249md[38;2;127;152;200md[38;2;243;174;146mO[38;2;218;154;131mk[38;2;162;159;114md[38;2;111;140;87ml[38;2;121;167;113md[38;2;155;167;147mx[38;2;217;197;183m0[38;2;163;136;121mo[38;2;107;122;154mc[38;2;83;134;199ml[38;2;84;135;203ml[38;2;85;139;209ml[38;2;85;137;209ml[38;2;82;130;194ml[38;2;87;136;201ml[38;2;87;134;196ml[38;2;75;107;154m:[38;2;90;132;192ml[38;2;94;153;238md[38;2;97;156;245md[38;2;92;131;188ml[38;2;98;151;233mo[38;2;101;154;236md[38;2;94;130;186ml[38;2;100;125;176ml[38;2;151;145;230md[38;2;164;148;237mx[38;2;164;148;237mxxx[0m\n"
        "[0m[38;2;164;148;237mx[38;2;164;148;237mx[38;2;151;135;207md[38;2;127;96;91m:[38;2;137;105;89mc[38;2;149;119;103ml[38;2;149;120;102ml[38;2;147;119;101ml[38;2;147;119;102ml[38;2;146;118;103ml[38;2;143;116;100ml[38;2;99;85;81m;[38;2;77;115;167m:[38;2;76;119;179mc[38;2;77;126;187mc[38;2;83;142;217mo[38;2;82;141;214ml[38;2;84;154;245mo[38;2;101;149;218mo[38;2;207;182;179mO[38;2;241;189;165m0[38;2;246;178;150m0[38;2;242;165;139mO[38;2;242;160;135mO[38;2;223;164;131mk[38;2;215;180;161mO[38;2;190;183;189mO[38;2;102;135;180ml[38;2;81;136;199ml[38;2;80;134;198ml[38;2;81;131;196ml[38;2;83;130;194ml[38;2;86;120;169mc[38;2;84;116;163mc[38;2;80;123;178mc[38;2;75;108;154m:[38;2;78;113;161m:[38;2;90;139;207ml[38;2;94;152;237mo[38;2;92;141;214mo[38;2;88;121;172mc[38;2;98;147;225mo[38;2;98;143;211mo[38;2;99;134;191ml[38;2;96;122;165mc[38;2;106;147;224mo[38;2;134;144;228md[38;2;161;146;234mx[38;2;164;148;237mx[38;2;164;148;237mx[0m\n"
        "[0m[38;2;162;147;234mx[38;2;145;127;182mo[38;2;131;106;102mc[38;2;148;119;103ml[38;2;148;120;104ml[38;2;146;119;103ml[38;2;145;118;104ml[38;2;144;118;104ml[38;2;144;118;105ml[38;2;132;107;96mc[38;2;90;102;130m:[38;2;70;109;163m:[38;2;79;132;195ml[38;2;74;118;180mc[38;2;75;123;184mc[38;2;78;132;196ml[38;2;80;139;211ml[38;2;81;153;245mo[38;2;111;143;197mo[38;2;219;187;178m0[38;2;240;194;170mK[38;2;249;195;163mK[38;2;249;193;162mK[38;2;248;191;161mK[38;2;248;190;160mK[38;2;223;175;155mO[38;2;177;157;160mx[38;2;159;146;156md[38;2;154;138;149mo[38;2;160;131;136mo[38;2;144;119;113ml[38;2;100;102;81m:[38;2;138;121;99ml[38;2;99;99;110m:[38;2;101;103;126m:[38;2;123;110;120mc[38;2;83;122;179mc[38;2;90;135;203ml[38;2;91;132;194ml[38;2;87;126;188mc[38;2;86;119;170mc[38;2;96;132;192ml[38;2;96;129;182ml[38;2;100;134;187ml[38;2;113;127;184ml[38;2;135;139;212md[38;2;132;147;231md[38;2;135;143;228md[38;2;155;144;229mx[38;2;164;148;236mx[0m\n"
        "[0m[38;2;150;123;127ml[38;2;154;124;110ml[38;2;146;119;103ml[38;2;145;118;104ml[38;2;144;118;105ml[38;2;143;118;105ml[38;2;142;117;104ml[38;2;141;114;102mc[38;2;112;103;108m:[38;2;79;110;157m:[38;2;67;109;163m:[38;2;71;115;173m:[38;2;70;114;172m:[38;2;67;108;164m:[38;2;72;115;179m:[38;2;74;124;189mc[38;2;79;137;210ml[38;2;82;150;237mo[38;2;90;127;188ml[38;2;175;166;181mk[38;2;235;184;157m0[38;2;248;194;164mK[38;2;249;195;164mK[38;2;249;195;164mK[38;2;235;181;152m0[38;2;244;193;161mK[38;2;249;193;162mK[38;2;249;190;160mK[38;2;248;182;153m0[38;2;234;152;124mk[38;2;189;138;106md[38;2;121;143;89ml[38;2;130;135;111ml[38;2;185;163;149mx[38;2;75;73;82m'[38;2;87;103;138m:[38;2;84;123;185mc[38;2;86;124;188mc[38;2;83;118;173mc[38;2;86;122;180mc[38;2;82;110;158m:[38;2;91;121;176mc[38;2;89;116;164mc[38;2;96;126;180ml[38;2;101;124;181ml[38;2;161;146;232mx[38;2;164;148;237mx[38;2;164;148;237mxxx[0m\n"
        "[0m[38;2;155;126;112mo[38;2;145;117;104ml[38;2;141;113;100mc[38;2;139;111;96mc[38;2;134;103;90mc[38;2;132;105;93mc[38;2;118;109;113mc[38;2;84;113;152m:[38;2;71;114;173m:[38;2;71;114;176m:[38;2;62;101;152m;[38;2;71;112;175m:[38;2;72;117;183mc[38;2;76;126;193mc[38;2;69;114;175m:[38;2;69;116;180m:[38;2;76;130;198ml[38;2;82;132;201ml[38;2;192;137;117md[38;2;223;161;133mk[38;2;247;190;162mK[38;2;248;192;165mK[38;2;248;192;165mK[38;2;248;192;165mK"/*[38;2;224;134;120mx[38;2;222;115;108md[38;2;203;117;100mo*/"[38;2;224;134;120ml[38;2;222;115;108mo[38;2;203;117;100ml"/*[38;2;224;134;120ml[38;2;222;115;108mo[38;2;203;117;100ml*/"[38;2;228;170;144mO[38;2;244;183;154m0[38;2;247;175;148m0[38;2;241;156;132mk[38;2;229;141;118mx[38;2;212;148;132mx[38;2;123;103;115mc[38;2;71;96;140m;[38;2;69;95;139m;[38;2;75;109;164m:[38;2;80;114;174mc[38;2;82;114;174mc[38;2;83;117;179mc[38;2;78;104;154m:[38;2;83;106;158m:[38;2;86;110;160m:[38;2;87;111;161m:[38;2;96;124;179ml[38;2;144;136;215md[38;2;164;148;237mx[38;2;164;148;237mxxx[0m\n"
        "[0m[38;2;146;119;106ml[38;2;129;102;89m:[38;2;141;124;120ml[38;2;167;161;165mx[38;2;164;162;167mx[38;2;184;183;188mO[38;2;155;156;166mx[38;2;80;117;167mc[38;2;69;113;172m:[38;2;69;111;171m:[38;2;64;105;158m:[38;2;71;118;183mc[38;2;71;122;188mc[38;2;76;137;206ml[38;2;76;139;212ml[38;2;69;119;183mc[38;2;68;113;173m:[38;2;75;127;192mc[38;2;169;131;124mo[38;2;208;141;113mx[38;2;213;141;115mx[38;2;235;173;146mO[38;2;248;192;165mK[38;2;248;192;165mK[38;2;241;185;157m0[38;2;239;174;148mO[38;2;248;191;161mK[38;2;248;192;162mK[38;2;247;191;161mK[38;2;245;187;158m0[38;2;243;180;154m0[38;2;212;152;138mk[38;2;113;113;149mc[38;2;73;110;172m:[38;2;74;109;170m:[38;2;71;103;162m:[38;2;78;119;194mc[38;2;80;117;185mc[38;2;81;118;185mc[38;2;82;107;164m:[38;2;84;99;145m:[38;2;86;102;156m:[38;2;87;105;160m:[38;2;84;103;157m:[38;2;87;110;164m:[38;2;138;132;207mo[38;2;164;148;237mx[38;2;164;148;237mxxx[0m\n"
        "[0m[38;2;116;89;75m;[38;2;153;158;170mx[38;2;209;221;241mX[38;2;199;204;214mK[38;2;200;205;215mK[38;2;203;209;220mK[38;2;188;192;203m0[38;2;153;155;165md[38;2;81;116;164mc[38;2;71;123;187mc[38;2;67;110;169m:[38;2;67;111;173m:[38;2;72;126;192mc[38;2;76;142;217ml[38;2;78;151;238mo[38;2;77;146;232mo[38;2;84;123;179mc[38;2;174;131;120mo[38;2;160;136;141mo[38;2;169;121;110mo[38;2;127;105;124mc[38;2;93;103;145m:[38;2;159;147;160md[38;2;208;170;160mO[38;2;205;168;160mk[38;2;190;160;157mk[38;2;178;157;160mx[38;2;172;156;163mx[38;2;166;152;163mx[38;2;150;141;159md[38;2;117;123;157ml[38;2;78;111;170m:[38;2;71;111;174m:[38;2;71;109;172m:[38;2;70;105;167m:[38;2;73;113;188m:[38;2;79;125;206mc[38;2;78;119;192mc[38;2;83;104;158m:[38;2;124;117;144ml[38;2;130;124;151ml[38;2;113;105;156mc[38;2;142;133;211mo[38;2;99;108;170mc[38;2;79;96;147m;[38;2;139;130;206mo[38;2;164;148;237mx[38;2;164;148;237mxxx[0m\n"
        "[0m[38;2;118;89;75m;[38;2;120;105;102m:[38;2;196;210;232mK[38;2;199;211;232mK[38;2;204;217;239mX[38;2;214;225;241mX[38;2;207;219;237mX[38;2;135;173;222mx[38;2;92;139;197ml[38;2;79;134;199ml[38;2;74;125;188mc[38;2;79;104;155m:[38;2;77;121;182mc[38;2;77;140;211ml[38;2;85;156;243mo[38;2;80;156;250mo[38;2;77;142;226ml[38;2;160;137;144mo[38;2;218;153;123mk[38;2;184;126;110mo[38;2;120;124;154ml[38;2;74;108;158m:[38;2;68;106;158m:[38;2;69;114;176m:[38;2;69;118;185mc[38;2;72;119;179mc[38;2;91;124;171mc[38;2;70;117;179mc[38;2;69;115;176m:[38;2;73;118;182mc[38;2;70;117;183mc[38;2;74;122;192mc[38;2;71;112;176m:[38;2;68;106;166m:[38;2;66;102;161m;[38;2;74;112;183m:[38;2;74;105;170m:[38;2;103;112;155mc[38;2;144;130;151mo[38;2;148;132;156mo[38;2;164;146;167md[38;2;144;129;203mo[38;2;164;148;237mx[38;2;163;148;234mx[38;2;108;109;173mc[38;2;130;128;203mo[38;2;160;145;230mx[38;2;163;147;235mx[38;2;164;148;237mx[38;2;164;148;237mx[0m\n"
        "[0m[38;2;116;87;74m;[38;2;126;94;81m:[38;2;129;120;122ml[38;2;199;212;237mK[38;2;201;215;241mK[38;2;190;207;233mK[38;2;163;192;230mO[38;2;169;186;211mO[38;2;188;190;197mO[38;2;127;160;205md[38;2;76;139;212ml[38;2;104;111;146mc[38;2;161;122;118ml[38;2;102;129;171ml[38;2;94;153;224mo[38;2;94;163;249md[38;2;80;156;251mo[38;2;116;141;191mo[38;2;177;141;141md[38;2;196;131;106md[38;2;200;133;106md[38;2;190;130;107md[38;2;146;124;132ml[38;2;107;102;118m:[38;2;106;125;160ml[38;2;124;140;168mo[38;2;155;158;170mx[38;2;82;121;177mc[38;2;68;114;174m:[38;2;64;105;161m:[38;2;73;126;197mc[38;2;77;127;199mc[38;2;71;117;184mc[38;2;67;105;166m:[38;2;72;104;160m:[38;2;118;124;156ml[38;2;88;104;151m:[38;2;156;142;161md[38;2;166;150;173mx[38;2;169;152;176mx[38;2;155;137;162md[38;2;155;140;221md[38;2;164;148;237mx[38;2;150;136;214md[38;2;160;144;231mx[38;2;164;148;237mx[38;2;164;148;237mx[38;2;164;148;237mx[38;2;164;148;237mxx[0m\n"
        "[0m[38;2;121;92;79m:[38;2;118;90;79m;[38;2;118;90;79m;[38;2;128;113;112mc[38;2;186;195;217m0[38;2;202;216;242mX[38;2;201;216;241mK[38;2;209;222;241mX[38;2;217;224;237mX[38;2;159;171;193mk[38;2;77;143;221ml[38;2;118;121;148ml[38;2;202;142;116mx[38;2;159;131;132mo[38;2;93;123;172mc[38;2;95;150;218mo[38;2;100;165;246md[38;2;81;156;250mo[38;2;126;150;199md[38;2;195;152;144mx[38;2;234;169;137mO[38;2;230;161;128mk[38;2;224;155;124mk[38;2;221;154;123mk[38;2;169;121;99ml[38;2;205;140;113mx[38;2;188;136;118md[38;2;157;136;140mo[38;2;89;125;182mc[38;2;73;132;210ml[38;2;77;138;218ml[38;2;72;122;189mc[38;2;68;112;176m:[38;2;64;103;162m;[38;2;99;118;160mc[38;2;157;147;163md[38;2;122;123;150ml[38;2;151;139;158md[38;2;170;158;177mx[38;2;169;156;177mx[38;2;150;136;162mo[38;2;163;147;232mx[38;2;164;148;237mx[38;2;164;148;237mx[38;2;164;148;237mxxxxxx[0m\n"
        "[0m[38;2;122;92;78m:[38;2;119;90;77m;[38;2;118;90;79m;[38;2;118;90;79m;[38;2;116;91;84m;[38;2;145;139;145mo[38;2;185;193;213m0[38;2;211;223;240mX[38;2;210;216;228mX[38;2;126;167;221mx[38;2;75;133;204ml[38;2;157;163;179mx[38;2;201;166;155mk[38;2;237;183;152m0[38;2;234;180;152m0[38;2;133;122;128ml[38;2;103;131;175ml[38;2;96;152;224mo[38;2;89;159;246md[38;2;111;143;198mo[38;2;199;153;138mx[38;2;228;160;128mk[38;2;231;162;130mk[38;2;230;161;129mk[38;2;201;143;116mx[38;2;202;145;117mx[38;2;227;162;129mk[38;2;207;141;113mx[38;2;157;120;123ml[38;2;107;126;166ml[38;2;70;118;181mc[38;2;67;115;180m:[38;2;69;115;178m:[38;2;85;104;143m:[38;2;157;153;170md[38;2;165;158;173mx[38;2;166;155;173mx[38;2;160;151;168md[38;2;165;153;173mx[38;2;169;157;177mx[38;2;139;127;163mo[38;2;164;148;237mx[38;2;164;148;237mxxxxxxxx[0m\n"
        "[0m[38;2;119;90;76m;[38;2;119;90;76m;[38;2;118;90;79m;[38;2;118;90;79m;[38;2;109;82;73m;[38;2;112;84;75m;[38;2;128;98;88m:[38;2;155;150;153md[38;2;158;172;193mk[38;2;89;160;246md[38;2;86;127;181mc[38;2;181;182;188mO[38;2;203;185;186mO[38;2;247;200;173mK[38;2;246;206;184mX[38;2;227;180;154mO[38;2;150;120;105ml[38;2;132;131;151mo[38;2;71;123;191mc[38;2;76;143;225ml[38;2;80;143;220ml[38;2;129;144;181mo[38;2;159;148;163md[38;2;220;168;145mO[38;2;238;174;143mO[38;2;187;137;112md[38;2;228;162;130mk[38;2;204;140;114md[38;2;181;146;137md[38;2;169;166;173mx[38;2;79;117;175mc[38;2;68;117;183m:[38;2;71;105;162m:[38;2;107;131;176ml[38;2;174;170;185mk[38;2;177;168;185mk[38;2;175;165;184mk[38;2;172;165;183mk[38;2;172;161;181mx[38;2;150;138;165md[38;2;151;136;216md[38;2;164;148;237mx[38;2;164;148;237mxxxxxxxx[0m\n"

        "[0m[38;2;163;149;234mCopyright (C) konacode | https://konacode.com/\033[0m\n\n");

    bool resizable = false, debug = false;
    if (argc>0) {
        bool log_to_file = false;
        for (int i = 1; i < argc; i++) {
            const std::string arg = argv[i];
            if (arg == "-debug" || arg == "--debug" || arg == "-d") debug = true;
            else if (arg == "-log" || arg == "--log" || arg == "-l") log_to_file = true;
            else if (arg == "-resizable" || arg == "--resizable" || arg == "-r") resizable = true;
            else if (arg == "-file" || arg == "--file" || arg == "-f") {
                try {
                    path = std::string(argv[++i]);
                } catch (std::exception &e) {
                    fprintf(stderr,"\033[31;1m[ERR]\033[0;31m Unable to store \"%s\" into a std::string! Error details: %s\033[0m\n",argv[i+1],e.what());
                } catch (...) {
                    fprintf(stderr,"\033[31;1m[ERR]\033[0;31m Unable to store \"%s\" into a std::string!\033[0m\n",argv[i+1]);
                }
            }
            else {
                printf("\033[33;1m[WRN]\033[0;33m Your command argument \"%s\" is invalid! Please use the following:\033[0m\n", argv[i]);
                printf("\033[33;1m[WRN]\033[0;33m \"--debug\" or \"-debug\" or \"-d\" to enable the debug logger output.\033[0m\n");
                printf("\033[33;1m[WRN]\033[0;33m \"--log\"   or \"-log\"   or \"-l\" to logging to file.\033[0m\n\n");
                printf("\033[33;1m[WRN]\033[0;33m \"--file\" + path   or \"-file\" + path   or \"-f\" + path to select a custom file to load.\033[0m\n\n");
            }
        }

        logger::initialize(log_to_file,debug);

    } else {
        logger::initialize();
    }

    logger::log("Starting Konata Dancer Remake version "+std::to_string(konacore::version[0])+"."+std::to_string(konacore::version[1])+"."+std::to_string(konacore::version[2])+" by konacode | https://konacode.com/\n");
    
    logger::log("Initializing...");
    if (!path.empty()) {
        logger::log("Konata Dancer will be loading \""+path.string()+"\".");
    } 
    // else { path = "./konata.gif"; }

    logger::log("Getting pixel data from STB...",logger::dbg);
    int iw,ih;
    if (!path.empty()) stbi_load(path.c_str(),&iw,&ih,nullptr,STBI_rgb_alpha);
    else iw = 640; ih = 480;

    logger::log("Loading animated GIF...", logger::dbg);
    konanix::g_raw_anim_data = load_gif_animation(path);
    konanix::image_size = iw*ih*STBI_rgb_alpha;
    
    try {
        konanix::initialize(konanix::g_raw_anim_data.frames.size(),iw, ih, debug, resizable);
    } catch (std::exception &e) {
        logger::log("Could not initialize Vulkan backend! Exception details: "+std::string(e.what()),logger::err);
        exit(1);
    }

    // logger::log("Uploading animated GIF...", logger::dbg);

    // for (size_t i = 0; i < konanix::g_raw_anim_data.frames.size(); ++i) {
    //     konanix::create_gif_image(konanix::g_raw_anim_data.frames[i].rgba.data(), i, konanix::g_raw_anim_data.width, konanix::g_raw_anim_data.height);
    //     logger::log("Loaded frame "+std::to_string(i),logger::dbg);
    // }
    // konanix::g_raw_anim_data.frames.clear();

    struct sigaction sigIntHandler {
        terminate_handler,
        {static_cast<unsigned long>(sigemptyset(&sigIntHandler.sa_mask))},
        0
    };

    sigaction(SIGINT, &sigIntHandler, NULL);
    size_t frame_index = 0;
    // auto next_frame_time = std::chrono::steady_clock::now() +
    // std::chrono::milliseconds(std::max(1, anim.frames[0].delay_ms));
    konanix::Overlay ctx; // context menu renderer
    int outdatedw = iw, outdatedh = ih;
    logger::log("Initialized!");
    logger::log("Started rendering loop!");
    while (!glfwWindowShouldClose(konanix::globals::window)) {
        // const auto now = std::chrono::steady_clock::now();
        // if (now >= next_frame_time) {
        std::this_thread::sleep_for(std::chrono::milliseconds(konanix::g_raw_anim_data.frames[frame_index].delay_ms));
            // w.upload_rgba_frame_to_gif_image(
            //     anim.frames[frame_index].rgba.data(),
            //     anim.frames[frame_index].rgba.size(),
            //     anim.width,
            //     anim.height,
            //     frame_index == 0
            // );
            frame_index = (frame_index + 1) % konanix::g_raw_anim_data.frames.size();
            // next_frame_time = now + std::chrono::milliseconds(std::max(1, anim.frames[frame_index].delay_ms));
        // }

        glfwGetFramebufferSize(konanix::globals::window, &ctx.w, &ctx.h);
            
        if (outdatedw != ctx.w || outdatedh != ctx.h) {
            konanix::recreate_swap_chain();
            outdatedw = ctx.w; outdatedh = ctx.h;
        }
        ctx.storage.assign(size_t(ctx.w) * size_t(ctx.h) * 4, 0);
        ctx.rgba = ctx.storage.data();
        ctx.clear();
        std::vector<uint8_t> overlay_buffer;
        overlay_buffer.assign(size_t(ctx.w) * size_t(ctx.h) * 4, 0);
        ctx.rgba = overlay_buffer.data();
        ctx.clear();

        const auto& src = konanix::g_raw_anim_data.frames[frame_index].rgba;
        const size_t copy_bytes = std::min(overlay_buffer.size(), src.size());
        memcpy(overlay_buffer.data(), src.data(), copy_bytes);
        // ctx.rgba = overlay_buffer.data();
        //
        // konanix::draw_context_menu(ctx);
        //
        // konanix::upload_rgba_frame_to_gif_image(
        //     ctx.rgba,
        //     overlay_buffer.size(),
        //     ctx.w,
        //     ctx.h,
        //     frame_index == 0
        // );
        konanix::draw_frame(ctx.w,ctx.h);
        glfwPollEvents();
    }

    konanix::cleanup();

    printf("\n[0m[38;2;0;0;0m                                          [38;2;21;21;21m [38;2;146;146;146mddddd[38;2;0;0;0m   [38;2;49;49;49m.[38;2;146;146;146md[38;2;97;97;97m;[38;2;0;0;0m  [38;2;65;65;65m'[38;2;146;146;146md[38;2;88;88;88m;[38;2;0;0;0m [38;2;88;88;88m;[38;2;146;146;146mdddddd[38;2;48;48;48m.[0m\n"
            "[0m[38;2;0;0;0m                                          [38;2;36;36;36m.[38;2;255;255;255mM[38;2;243;243;243mW[38;2;234;234;234m [38;2;178;178;178mO[38;2;255;255;255mM[38;2;217;217;217mX[38;2;18;18;18m [38;2;0;0;0m [38;2;140;140;140m [38;2;232;232;232mN[38;2;237;237;237mN[38;2;24;24;24m [38;2;6;6;6m [38;2;220;220;220mX[38;2;255;255;255mM[38;2;18;18;18m [38;2;0;0;0m [38;2;154;154;154mx[38;2;255;255;255mM[38;2;189;189;189m0[38;2;234;234;234m    [38;2;140;140;140m [0m\n"
            "[0m[38;2;0;0;0m                                          [38;2;36;36;36m.[38;2;255;255;255mMM[38;2;21;21;21m [38;2;188;188;188mO[38;2;255;255;255mMM[38;2;30;30;30m.[38;2;0;0;0m  [38;2;50;50;50m.[38;2;255;255;255mM[38;2;230;230;230mN[38;2;221;221;221mX[38;2;255;255;255mM[38;2;99;99;99m;[38;2;34;34;34m [38;2;0;0;0m [38;2;154;154;154mx[38;2;255;255;255mM[38;2;208;208;208mK[38;2;21;21;21m   [38;2;4;4;4m [38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m         [38;2;20;20;20m [38;2;0;0;0m      [38;2;11;11;16m [38;2;31;33;46m.[38;2;48;49;65m.[38;2;34;34;47m.[38;2;5;5;6m [38;2;46;46;46m [38;2;32;32;32m [38;2;23;23;23m [38;2;18;18;18m [38;2;2;2;2m [38;2;0;0;0m                [38;2;36;36;36m.[38;2;255;255;255mMMMMMM[38;2;146;146;146mo[38;2;0;0;0m  [38;2;51;51;51m [38;2;142;142;142mo[38;2;255;255;255mMM[38;2;195;195;195m0[38;2;76;76;76m [38;2;0;0;0m  [38;2;154;154;154mx[38;2;255;255;255mMMMMM[38;2;48;48;48m.[38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m             [38;2;20;23;31m [38;2;70;82;116m,[38;2;94;115;172mc[38;2;11;13;20m [38;2;91;91;91m [38;2;44;44;44m [38;2;31;31;31m [38;2;16;16;16m [38;2;0;0;0m    [38;2;23;23;23m [38;2;12;12;12m [38;2;17;17;17m [38;2;0;0;0m              [38;2;36;36;36m.[38;2;255;255;255mM[38;2;243;243;243mW[38;2;64;64;64m [38;2;15;15;15m [38;2;238;238;238mW[38;2;255;255;255mM[38;2;243;243;243mW[38;2;0;0;0m   [38;2;10;10;10m [38;2;255;255;255mMM[38;2;34;34;34m.[38;2;0;0;0m   [38;2;154;154;154mx[38;2;255;255;255mM[38;2;163;163;163mx[38;2;64;64;64m   [38;2;32;32;32m [38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m           [38;2;6;6;6m [38;2;64;75;111m,[38;2;109;135;205mo[38;2;95;123;187ml[38;2;82;82;82m [38;2;1;1;1m [38;2;0;0;0m [38;2;8;8;8m [38;2;0;0;0m     [38;2;10;10;10m [38;2;0;0;0m  [38;2;14;14;14m [38;2;12;12;12m [38;2;0;0;0m  [38;2;3;3;3m [38;2;15;15;18m [38;2;7;7;7m [38;2;0;0;0m  [38;2;31;31;32m.[38;2;45;46;55m.[38;2;15;15;18m [38;2;28;28;28m [38;2;0;0;0m  [38;2;36;36;36m.[38;2;255;255;255mMM[38;2;191;191;191m0[38;2;202;202;202m0[38;2;255;255;255mMM[38;2;64;64;64m'[38;2;0;0;0m   [38;2;5;5;5m [38;2;255;255;255mMM[38;2;25;25;25m [38;2;0;0;0m   [38;2;154;154;154mx[38;2;255;255;255mM[38;2;242;242;242mW[38;2;191;191;191m0000[38;2;64;64;64m'[0m\n"
            "[0m[38;2;0;0;0m           [38;2;21;21;21m [38;2;89;106;159m:[38;2;107;138;214mo[38;2;81;94;129m;[38;2;0;0;0m     [38;2;4;4;4m [38;2;28;28;34m.[38;2;46;48;59m.[38;2;45;47;58m.[38;2;42;45;57m.[38;2;33;37;50m.[38;2;25;28;40m [38;2;24;26;35m [38;2;33;39;57m.[38;2;49;62;96m.[38;2;73;96;148m;[38;2;97;125;193ml[38;2;106;132;199ml[38;2;58;65;90m'[38;2;86;96;125m;[38;2;104;121;173ml[38;2;84;95;132m;[38;2;102;102;102m [38;2;29;29;29m [38;2;0;0;0m   [38;2;51;51;51m [38;2;128;128;128m     [38;2;111;111;111m [38;2;17;17;17m [38;2;0;0;0m   [38;2;21;21;21m [38;2;128;128;128m [38;2;13;12;12m [38;2;35;22;22m [38;2;2;0;0m [38;2;0;0;0m  [38;2;119;119;119m [38;2;149;149;149m      [38;2;89;89;89m [0m\n"
            "[0m[38;2;0;0;0m         [38;2;12;12;12m [38;2;0;0;0m  [38;2;47;47;47m [38;2;99;99;99m [38;2;57;62;78m.[38;2;14;14;14m [38;2;38;40;49m.[38;2;66;74;100m'[38;2;71;87;129m;[38;2;88;109;163m:[38;2;101;127;194ml[38;2;103;135;209mo[38;2;106;139;215mo[38;2;105;140;217mo[38;2;105;141;219mo[38;2;105;142;220mo[38;2;105;141;220mo[38;2;104;139;215mo[38;2;105;141;220mo[38;2;104;141;221mo[38;2;102;141;220mo[38;2;103;140;220mo[38;2;105;141;219mo[38;2;89;115;183mc[38;2;105;141;220mo[38;2;102;139;218mo[38;2;109;140;210mo[38;2;108;132;192ml[38;2;113;136;193mo[38;2;85;106;162m:[38;2;78;95;141m;[38;2;78;92;127m;[38;2;63;69;91m'[38;2;32;32;39m.[38;2;4;4;4m [38;2;0;0;0m        [38;2;25;0;0m [38;2;179;0;0m.[38;2;233;0;0m'[38;2;250;0;0m'[38;2;240;0;0m'[38;2;202;0;0m.[38;2;89;0;0m.[38;2;0;0;0m [38;2;0;0;0m [38;2;2;2;2m [38;2;9;0;0m [38;2;1;0;0m [38;2;0;0;0m   [0m\n"
            "[0m[38;2;0;0;0m           [38;2;7;7;7m [38;2;0;0;0m [38;2;14;15;20m [38;2;87;98;130m;[38;2;108;134;198mo[38;2;106;140;214mo[38;2;104;142;221mo[38;2;103;139;220mo[38;2;103;139;220moo[38;2;102;138;217mo[38;2;103;141;220mo[38;2;104;141;221mo[38;2;104;142;220mo[38;2;101;137;214mo[38;2;104;142;221mo[38;2;101;138;217mo[38;2;103;140;220mo[38;2;103;140;220mo[38;2;103;140;220mo[38;2;103;139;220mo[38;2;104;139;219mo[38;2;92;121;194mc[38;2;105;142;221mo[38;2;104;142;221mo[38;2;102;141;219mo[38;2;102;140;220mo[38;2;103;140;220mo[38;2;102;140;219mo[38;2;102;141;219mo[38;2;102;140;219mo[38;2;106;139;218mo[38;2;109;134;205mo[38;2;92;111;166mc[38;2;63;73;107m'[38;2;35;38;52m.[38;2;1;1;2m [38;2;0;0;0m    [38;2;3;3;3m [38;2;210;0;0m'[38;2;255;0;0m,,,,,[38;2;253;0;0m,[38;2;159;0;0m.[38;2;192;0;0m.[38;2;237;0;0m'[38;2;249;0;0m'[38;2;240;0;0m'[38;2;195;0;0m.[38;2;63;0;0m [38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m       [38;2;4;4;4m [38;2;0;0;0m   [38;2;16;17;20m [38;2;93;106;147m:[38;2;114;139;202mo[38;2;107;141;216mo[38;2;104;142;221mo[38;2;103;142;220mo[38;2;104;141;221mo[38;2;103;141;220mo[38;2;103;141;220mo[38;2;95;130;206ml[38;2;97;132;209ml[38;2;104;141;221mo[38;2;103;140;219mo[38;2;100;128;196ml[38;2;94;123;193ml[38;2;103;138;213mo[38;2;92;126;200ml[38;2;102;140;217mo[38;2;102;140;219mo[38;2;104;141;220mo[38;2;103;140;220mo[38;2;98;128;200ml[38;2;100;121;183mc[38;2;105;142;221mo[38;2;104;142;221mo[38;2;102;141;220mo[38;2;102;140;219mo[38;2;103;140;218mo[38;2;117;147;220md[38;2;141;158;206mx[38;2;76;83;106m,[38;2;68;68;68m [38;2;85;85;85m [38;2;89;89;89m [38;2;1;1;2m [38;2;1;1;1m [38;2;64;64;64m [38;2;36;36;36m [38;2;2;2;2m [38;2;0;0;0m  [38;2;3;3;3m [38;2;149;0;0m.[38;2;255;0;0m,,,,,,,,,,,,[38;2;232;0;0m'[38;2;6;6;6m [0m\n"
            "[0m[38;2;0;0;0m        [38;2;14;14;14m [38;2;0;0;0m [38;2;15;15;19m [38;2;101;121;176mc[38;2;150;169;220mk[38;2;192;206;243mK[38;2;136;160;218mx[38;2;125;157;225md[38;2;119;151;220md[38;2;115;146;220md[38;2;116;146;218md[38;2;104;134;205ml[38;2;84;110;178mc[38;2;106;139;214mo[38;2;108;137;211mo[38;2;136;145;190md[38;2;189;179;184mO[38;2;105;123;178ml[38;2;100;131;204ml[38;2;111;142;216mo[38;2;111;145;220mo[38;2;110;145;220mo[38;2;105;142;220mo[38;2;105;141;221mo[38;2;123;134;174mo[38;2;193;181;184mO[38;2;112;136;198mo[38;2;121;153;224md[38;2;118;146;213md[38;2;150;174;231mk[38;2;172;191;236mO[38;2;194;208;246mK[38;2;202;213;247mK[38;2;189;196;222m0[38;2;100;98;107m;[38;2;33;33;33m [38;2;0;0;0m [38;2;17;17;17m [38;2;3;3;3m [38;2;4;4;4m [38;2;0;0;0m [38;2;20;20;20m [38;2;3;3;3m [38;2;0;0;0m  [38;2;29;29;29m [38;2;189;0;0m.[38;2;255;0;0m,,,,,,,,,,,[38;2;47;0;0m [38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m         [38;2;2;2;2m [38;2;102;104;134m:[38;2;159;174;218mk[38;2;186;200;240m0[38;2;159;175;220mk[38;2;136;157;210mx[38;2;137;163;226mx[38;2;126;154;223md[38;2;118;148;219md[38;2;96;125;193ml[38;2;69;94;160m;[38;2;100;127;197ml[38;2;106;131;197ml[38;2;165;159;176mx[38;2;224;196;176m0[38;2;218;195;177m0[38;2;102;122;178ml[38;2;104;141;218mo[38;2;102;140;216mo[38;2;102;140;218mo[38;2;102;140;219mo[38;2;104;142;221mo[38;2;104;134;206ml[38;2;175;164;168mk[38;2;220;191;174m0[38;2;159;146;149md[38;2;107;119;172mc[38;2;92;119;188mc[38;2;119;148;216md[38;2;135;162;226mx[38;2;148;172;229mk[38;2;158;176;224mk[38;2;182;197;239m0[38;2;189;199;234m0[38;2;53;54;61m.[38;2;0;0;0m          [38;2;24;24;24m [38;2;97;0;0m.[38;2;255;0;0m,,,,,,,[38;2;203;0;0m'[38;2;2;0;0m [38;2;36;36;36m [38;2;4;4;4m [38;2;0;0;0m [0m\n"
            "[0m[38;2;0;0;0m         [38;2;23;26;33m [38;2;106;127;190ml[38;2;113;142;215mo[38;2;99;130;202ml[38;2;90;121;193mc[38;2;104;141;220mo[38;2;103;141;219mo[38;2;105;142;221mo[38;2;94;127;201ml[38;2;83;111;182mc[38;2;94;123;196ml[38;2;118;141;199mo[38;2;199;187;189m0[38;2;238;208;186mX[38;2;237;206;184mK[38;2;224;199;180mK[38;2;106;124;181ml[38;2;104;141;220mo[38;2;102;140;216mo[38;2;102;140;217mo[38;2;98;131;205ml[38;2;98;130;202ml[38;2;125;138;180mo[38;2;237;206;187mK[38;2;236;206;184mK[38;2;235;206;186mK[38;2;145;138;156mo[38;2;103;124;190ml[38;2;95;124;197ml[38;2;102;140;217mo[38;2;103;140;219mo[38;2;91;124;197ml[38;2;101;134;209ml[38;2;106;141;217mo[38;2;113;138;200mo[38;2;26;26;30m [38;2;11;11;11m [38;2;2;2;2m [38;2;0;0;0m        [38;2;11;11;11m [38;2;67;0;0m [38;2;255;0;0m,,[38;2;147;0;0m.[38;2;62;62;62m [38;2;47;47;47m [38;2;32;32;32m [38;2;15;15;15m [38;2;0;0;0m    [0m\n"
            "[0m[38;2;0;0;0m      [38;2;9;9;9m [38;2;0;0;0m [38;2;7;8;12m [38;2;99;119;180mc[38;2;101;130;204ml[38;2;86;113;182mc[38;2;88;116;186mc[38;2;105;139;215mo[38;2;102;140;217mo[38;2;102;140;218mo[38;2;102;134;211mo[38;2;92;120;191mc[38;2;80;94;141m;[38;2;136;136;164mo[38;2;182;159;150mx[38;2;185;160;145mx[38;2;181;153;138mx[38;2;192;164;148mk[38;2;225;199;181mK[38;2;101;103;133m:[38;2;104;137;212mo[38;2;103;140;219mo[38;2;105;133;203ml[38;2;155;156;181mx[38;2;115;120;166ml[38;2;193;174;172mO[38;2;171;144;131md[38;2;168;143;128md[38;2;178;150;136mx[38;2;198;173;163mk[38;2;121;121;155ml[38;2;97;120;190mc[38;2;102;140;218mo[38;2;102;140;216mo[38;2;88;120;191mc[38;2;76;106;175m:[38;2;101;139;215mo[38;2;103;141;219mo[38;2;99;111;156mc[38;2;39;39;39m [38;2;0;0;0m       [38;2;17;17;17m [38;2;63;60;60m.[38;2;45;42;42m.[38;2;49;21;20m [38;2;52;52;52m [38;2;29;29;29m [38;2;9;9;9m [38;2;0;0;0m        [0m\n"
            "[0m[38;2;0;0;0m      [38;2;3;3;3m [38;2;3;3;3m [38;2;68;82;125m;[38;2;3;3;5m [38;2;97;120;183ml[38;2;84;111;180mc[38;2;93;126;198ml[38;2;102;140;216mo[38;2;102;140;216mo[38;2;100;136;212mo[38;2;89;113;180mc[38;2;77;77;100m,[38;2;91;80;76m,[38;2;51;57;38m.[38;2;42;58;33m.[38;2;43;61;35m.[38;2;45;63;35m.[38;2;55;65;41m.[38;2;204;181;166mO[38;2;127;125;149ml[38;2;109;140;220mo[38;2;117;139;203mo[38;2;187;180;189mO[38;2;217;193;179m0[38;2;152;136;140mo[38;2;174;155;139mx[38;2;50;63;39m.[38;2;49;68;40m.[38;2;44;66;37m.[38;2;63;68;50m'[38;2;98;86;88m;[38;2;87;93;131m;[38;2;106;139;215mo[38;2;102;140;216mo[38;2;101;133;206ml[38;2;65;89;153m;[38;2;96;127;199ml[38;2;106;143;221mo[38;2;101;127;194ml[38;2;1;2;2m [38;2;0;0;0m      [38;2;31;30;29m [38;2;191;171;160mk[38;2;221;197;181m0[38;2;214;188;172m0[38;2;206;184;169mO[38;2;29;27;26m [38;2;49;49;49m [38;2;7;7;7m [38;2;0;0;0m        [0m\n"
            "[0m[38;2;0;0;0m       [38;2;11;12;16m [38;2;65;65;65m [38;2;22;22;22m [38;2;104;125;188ml[38;2;79;105;173m:[38;2;108;140;217mo[38;2;102;140;219mo[38;2;102;140;219mo[38;2;94;128;204ml[38;2;85;104;164m:[38;2;158;150;156md[38;2;211;211;205mK[38;2;146;166;144mx[38;2;137;167;135md[38;2;60;111;60m;[38;2;76;126;75m:[38;2;65;100;57m,[38;2;225;204;183mK[38;2;135;132;156mo[38;2;132;145;195md[38;2;211;193;188m0[38;2;236;208;184mK[38;2;220;190;170m0[38;2;225;193;175m0[38;2;199;175;158mO[38;2;150;169;145mx[38;2;89;132;86mc[38;2;55;105;55m,[38;2;86;112;83m:[38;2;165;154;145md[38;2;124;121;139ml[38;2;96;123;193ml[38;2;102;140;218mo[38;2;88;116;184mc[38;2;1;1;1m [38;2;23;27;39m [38;2;105;142;219mo[38;2;105;134;202ml[38;2;3;3;3m [38;2;5;5;5m [38;2;0;0;0m  [38;2;9;9;9m [38;2;71;63;60m'[38;2;144;127;120ml[38;2;215;193;182m0[38;2;234;203;184mK[38;2;235;202;183mK[38;2;233;203;186mK[38;2;58;52;50m.[38;2;56;56;56m [38;2;8;8;8m [38;2;17;17;17m [38;2;0;0;0m        [0m\n"
            "[0m[38;2;0;0;0m       [38;2;9;9;9m [38;2;0;0;0m [38;2;51;51;51m [38;2;93;113;172mc[38;2;90;115;183mc[38;2;106;140;219mo[38;2;102;140;219mo[38;2;95;130;207ml[38;2;74;104;174m:[38;2;107;129;193ml[38;2;196;183;179mO[38;2;242;232;225mN[38;2;141;143;139mo[38;2;121;151;116mo[38;2;126;179;122mx[38;2;124;177;120md[38;2;81;108;72m;[38;2;227;206;186mK[38;2;187;163;153mk[38;2;230;203;188mK[38;2;237;206;185mK[38;2;237;206;185mK[38;2;237;206;185mK[38;2;237;205;186mK[38;2;187;164;147mk[38;2;101;126;95mc[38;2;129;181;125mx[38;2;81;130;79m:[38;2;163;171;151mx[38;2;214;202;192mK[38;2;88;97;138m:[38;2;69;96;164m;[38;2;104;138;212mo[38;2;41;47;70m.[38;2;7;7;7m [38;2;18;18;18m [38;2;32;39;58m.[38;2;106;138;211mo[38;2;2;2;3m [38;2;6;5;5m [38;2;90;81;75m,[38;2;133;117;107mc[38;2;189;166;153mk[38;2;234;201;185mK[38;2;236;203;186mK[38;2;235;204;185mK[38;2;41;36;33m.[38;2;136;136;136m [38;2;81;81;81m [38;2;19;19;19m [38;2;0;0;0m           [0m\n"
            "[0m[38;2;0;0;0m      [38;2;12;12;12m [38;2;0;0;0m  [38;2;51;51;51m [38;2;86;93;125m;[38;2;124;124;148ml[38;2;108;140;220mo[38;2;94;126;201ml[38;2;72;101;170m:[38;2;76;106;175m:[38;2;107;138;211mo[38;2;178;168;176mk[38;2;235;208;188mK[38;2;209;189;171m0[38;2;161;153;127md[38;2;179;169;145mk[38;2;201;186;162mO[38;2;229;202;182mK[38;2;237;206;186mK[38;2;237;206;186mK[38;2;236;205;187mK[38;2;237;206;186mK[38;2;238;206;186mK[38;2;238;206;185mK[38;2;237;206;186mK[38;2;233;204;184mK[38;2;204;181;162mO[38;2;183;171;147mk[38;2;175;167;142mx[38;2;220;195;178m0[38;2;179;163;155mx[38;2;84;102;161m:[38;2;81;106;172m:[38;2;98;120;183mc[38;2;63;64;82m'[38;2;7;7;7m [38;2;27;27;27m [38;2;26;26;26m [38;2;98;102;134m:[38;2;106;101;106m:[38;2;210;182;166mO[38;2;235;205;185mK[38;2;236;206;185mK[38;2;235;205;184mK[38;2;227;199;181mK[38;2;11;10;9m [38;2;79;79;79m [38;2;9;9;9m [38;2;0;0;0m              [0m\n"
            "[0m[38;2;0;0;0m       [38;2;3;3;3m [38;2;5;5;5m [38;2;18;18;18m [38;2;72;79;104m,[38;2;108;122;176ml[38;2;103;124;191ml[38;2;97;113;164mc[38;2;90;118;186mc[38;2;78;107;177m:[38;2;105;138;215mo[38;2;149;147;176md[38;2;237;206;184mK[38;2;238;206;185mK[38;2;237;205;187mK[38;2;236;205;186mK[38;2;236;205;186mK[38;2;237;206;186mK[38;2;217;179;166mO[38;2;211;162;156mk[38;2;211;160;156mk[38;2;203;150;148mx[38;2;209;154;153mk[38;2;215;170;162mO[38;2;238;205;188mK[38;2;236;206;185mKK[38;2;237;205;186mK[38;2;236;205;187mK[38;2;237;205;186mK[38;2;163;148;153md[38;2;93;120;190mc[38;2;105;130;197ml[38;2;77;78;111m,[38;2;109;112;147mc[38;2;88;93;133m;[38;2;75;69;77m'[38;2;205;183;174mO[38;2;225;192;173m0[38;2;220;191;172m0[38;2;235;205;184mK[38;2;238;206;185mK[38;2;139;122;110ml[38;2;133;133;133m [38;2;54;54;54m [38;2;3;3;3m [38;2;0;0;0m                [0m\n"
            "[0m[38;2;0;0;0m        [38;2;3;3;3m [38;2;12;12;12m [38;2;31;35;47m.[38;2;104;130;202ml[38;2;88;107;170m:[38;2;90;114;182mc[38;2;102;132;206ml[38;2;78;108;178m:[38;2;104;135;214mo[38;2;111;125;181ml[38;2;191;173;173mk[38;2;217;190;177m0[38;2;237;205;186mK[38;2;237;205;186mK[38;2;236;205;185mK[38;2;235;201;182mK[38;2;186;135;130md[38;2;214;148;155mk[38;2;218;148;156mk[38;2;216;148;155mk[38;2;215;148;154mk[38;2;188;138;134md[38;2;238;205;188mK[38;2;236;206;186mK[38;2;236;207;187mK[38;2;220;193;174m0[38;2;184;163;158mk[38;2;145;143;163md[38;2;97;117;171mc[38;2;107;140;218mo[38;2;96;113;175mc[38;2;159;156;173mx[38;2;130;128;141ml[38;2;133;135;167mo[38;2;97;102;149m:[38;2;136;131;152mo[38;2;219;203;194mK[38;2;175;156;141mx[38;2;22;20;18m [38;2;76;76;76m [38;2;22;22;22m [38;2;0;0;0m                   [0m\n"
            "[0m[38;2;0;0;0m          [38;2;1;2;2m [38;2;81;105;168m:[38;2;105;138;213mo[38;2;98;128;201ml[38;2;91;123;197ml[38;2;92;126;199ml[38;2;97;130;204ml[38;2;100;133;206ml[38;2;68;95;160m;[38;2;75;99;157m:[38;2;89;96;131m;[38;2;122;128;159ml[38;2;141;134;151mo[38;2;159;143;145md[38;2;172;143;132md[38;2;185;143;134md[38;2;186;141;133md[38;2;159;128;123mo[38;2;148;125;130ml[38;2;149;131;139mo[38;2;136;124;138ml[38;2;151;140;153md[38;2;118;108;122mc[38;2;125;120;135ml[38;2;70;81;126m,[38;2;77;108;176m:[38;2;102;138;215mo[38;2;106;140;216mo[38;2;95;110;161mc[38;2;101;102;137m:[38;2;156;156;176mx[38;2;144;142;156mo[38;2;123;123;147ml[38;2;115;115;149mc[38;2;74;72;90m'[38;2;42;42;42m [38;2;11;11;11m [38;2;0;0;0m                     [0m\n"
            "[0m[38;2;0;0;0m          [38;2;29;32;44m.[38;2;105;131;203ml[38;2;100;135;209ml[38;2;80;111;181m:[38;2;79;108;178m:[38;2;103;140;216mo[38;2;105;139;216mo[38;2;103;136;211mo[38;2;74;101;167m:[38;2;67;94;158m;[38;2;71;95;155m;[38;2;65;88;148m;[38;2;64;73;120m,[38;2;72;72;108m,[38;2;162;130;125mo[38;2;203;151;133mx[38;2;203;151;134mx[38;2;147;118;121ml[38;2;91;87;113m;[38;2;72;78;126m,[38;2;72;77;123m,[38;2;132;135;166mo[38;2;122;120;142ml[38;2;116;117;147mc[38;2;64;80;133m,[38;2;86;112;178mc[38;2;108;143;220mo[38;2;101;130;202ml[38;2;206;207;222mK[38;2;200;199;212m0[38;2;112;113;147mc[38;2;135;134;160mo[38;2;59;57;60m.[38;2;80;80;80m [38;2;32;32;32m [38;2;0;0;0m                       [0m\n"
            "[0m[38;2;0;0;0m         [38;2;4;5;6m [38;2;98;119;178mc[38;2;105;141;218mo[38;2;81;113;182mc[38;2;74;104;171m:[38;2;67;96;162m;[38;2;102;136;210mo[38;2;104;139;220mo[38;2;88;116;188mc[38;2;79;92;145m;[38;2;72;84;132m,[38;2;83;94;146m;[38;2;73;86;140m;[38;2;71;78;125m,[38;2;71;77;120m,[38;2;122;103;115m:[38;2;187;142;134md[38;2;189;148;140mx[38;2;190;152;146mx[38;2;137;116;121ml[38;2;79;83;129m,[38;2;114;118;157mc[38;2;105;101;123m:[38;2;173;172;190mk[38;2;84;83;117m;[38;2;129;132;155mo[38;2;97;123;192ml[38;2;109;142;223mo[38;2;119;137;192mo[38;2;245;241;239mW[38;2;246;241;239mW[38;2;232;225;226mN[38;2;36;34;36m.[38;2;16;16;16m [38;2;0;0;0m                         [0m\n"
            "[0m[38;2;0;0;0m         [38;2;65;76;111m,[38;2;108;140;214mo[38;2;89;121;194mc[38;2;73;104;171m:[38;2;73;104;170m:[38;2;73;104;170m:[38;2;76;105;174m:[38;2;108;140;217mo[38;2;96;125;198ml[38;2;111;112;140mc[38;2;102;99;123m:[38;2;112;112;146mc[38;2;109;115;160mc[38;2;86;97;149m:[38;2;80;89;142m;[38;2;76;81;127m,[38;2;166;167;191mk[38;2;198;197;215m0[38;2;202;199;215m0[38;2;172;169;189mk[38;2;94;99;144m:[38;2;133;134;163mo[38;2;142;140;161mo[38;2;113;115;152mc[38;2;114;113;148mc[38;2;233;232;240mN[38;2;103;124;192ml[38;2;103;132;209ml[38;2;195;197;218m0[38;2;248;245;242mW[38;2;248;244;244mW[38;2;64;61;61m.[38;2;39;39;39m [38;2;0;0;0m                          [0m\n"
            "[0m[38;2;0;0;0m        [38;2;7;8;10m [38;2;101;131;201ml[38;2;97;132;206ml[38;2;73;106;173m:[38;2;73;104;170m:[38;2;73;104;170m:[38;2;73;104;170m:[38;2;70;100;165m:[38;2;87;116;187mc[38;2;102;136;210mo[38;2;99;105;145m:[38;2;111;112;151mc[38;2;138;139;170mo[38;2;122;121;146ml[38;2;117;118;144mc[38;2;122;128;163ml[38;2;94;104;153m:[38;2;106;110;154mc[38;2;222;220;231mX[38;2;242;239;238mW[38;2;203;196;198m0[38;2;137;137;163mo[38;2;108;107;138mc[38;2;138;139;167mo[38;2;93;97;147m:[38;2;189;188;206mO[38;2;236;233;239mN[38;2;95;114;169mc[38;2;154;163;196mx[38;2;159;151;146md[38;2;155;155;155m [38;2;91;91;91m [38;2;18;18;18m [38;2;0;0;0m                           [0m\n"
            "[0m[38;2;0;0;0m        [38;2;56;66;94m'[38;2;106;139;217mo[38;2;77;109;176m:[38;2;73;104;170m:[38;2;73;104;170m:[38;2;73;104;169m:[38;2;78;104;163m:[38;2;142;154;186md[38;2;212;215;232mX[38;2;125;141;197mo[38;2;158;161;185mx[38;2;222;219;226mX[38;2;168;167;190mk[38;2;132;130;157mo[38;2;126;125;154ml[38;2;123;121;140ml[38;2;132;131;153mo[38;2;136;136;166mo[38;2;132;131;152mo[38;2;244;241;240mW[38;2;184;181;184mO[38;2;112;108;132mc[38;2;126;124;148ml[38;2;94;98;142m:[38;2;126;128;161ml[38;2;216;215;225mX[38;2;151;155;182mx[38;2;69;79;127m,[38;2;89;110;164m:[38;2;55;53;68m.[38;2;27;27;27m [38;2;0;0;0m                             [0m\n"
            "[0m[38;2;0;0;0m       [38;2;26;26;26m [38;2;92;114;170mc[38;2;91;124;196ml[38;2;72;105;172m:[38;2;71;102;169m:[38;2;71;102;168m:[38;2;145;157;190mx[38;2;222;220;227mX[38;2;245;242;239mW[38;2;244;241;239mW[38;2;229;228;229mN[38;2;174;173;181mk[38;2;246;242;241mW[38;2;246;242;243mW[38;2;237;234;237mN[38;2;186;183;197mO[38;2;143;141;164mo[38;2;119;118;147ml[38;2;128;126;140ml[38;2;124;122;135ml[38;2;188;185;189mO[38;2;146;141;152mo[38;2;115;114;140mc[38;2;103;104;145m:[38;2;111;115;156mc[38;2;230;228;233mN[38;2;148;149;170md[38;2;74;103;165m:[38;2;73;104;169m:[38;2;73;105;170m:[38;2;43;47;67m.[38;2;0;0;0m                              [0m\n"
            "[0m[38;2;0;0;0m       [38;2;19;20;27m [38;2;103;136;210mo[38;2;76;106;173m:[38;2;63;91;157m;[38;2;72;103;169m:[38;2;73;100;161m:[38;2;176;181;204mO[38;2;242;237;238mW[38;2;243;242;242mW[38;2;243;242;242mW[38;2;244;241;242mW[38;2;242;241;242mW[38;2;242;242;243mW[38;2;243;242;242mW[38;2;244;241;242mW[38;2;245;242;242mW[38;2;243;241;244mW[38;2;203;203;216mK[38;2;147;145;166md[38;2;128;126;151ml[38;2;123;121;137ml[38;2;119;115;132mc[38;2;97;98;138m:[38;2;117;119;157ml[38;2;230;227;234mN[38;2;246;241;238mW[38;2;211;210;220mK[38;2;77;98;156m:[38;2;72;105;171m:[38;2;73;104;169m:[38;2;40;45;66m.[38;2;0;0;0m                              [0m\n"
            "[0m[38;2;0;0;0m       [38;2;60;73;109m,[38;2;92;124;196ml[38;2;60;81;129m,[38;2;45;64;110m.[38;2;74;100;161m:[38;2;96;103;136m:[38;2;158;158;174mx[38;2;209;206;209mK[38;2;242;240;242mW[38;2;242;241;242mW[38;2;244;242;243mW[38;2;245;241;242mW[38;2;245;242;243mW[38;2;246;242;242mW[38;2;233;230;231mN[38;2;244;242;243mW[38;2;244;241;242mW[38;2;234;228;226mN[38;2;225;218;208mX[38;2;212;206;194mK[38;2;150;148;154md[38;2;105;107;131m:[38;2;119;120;146ml[38;2;202;197;199m0[38;2;232;224;216mX[38;2;236;230;226mN[38;2;243;239;244mW[38;2;94;108;158m:[38;2;72;105;172m:[38;2;73;104;169m:[38;2;59;67;96m'[38;2;0;0;0m                              [0m\n"
            "[0m[38;2;0;0;0m      [38;2;2;2;3m [38;2;98;121;186ml[38;2;77;106;173m:[38;2;10;11;16m [38;2;65;82;134m,[38;2;79;94;147m;[38;2;133;131;156mo[38;2;120;117;136mc[38;2;138;135;149mo[38;2;185;183;192mO[38;2;214;212;217mK[38;2;243;242;243mW[38;2;245;240;243mW[38;2;225;221;224mX[38;2;226;221;222mX[38;2;237;234;236mN[38;2;230;222;216mX[38;2;224;212;178mK[38;2;230;216;164mK[38;2;241;229;165mX[38;2;241;229;164mX[38;2;241;229;165mX[38;2;241;228;167mX[38;2;241;229;167mX[38;2;241;229;166mX[38;2;241;228;167mX[38;2;225;211;163mK[38;2;220;211;205mK[38;2;141;146;178md[38;2;74;104;171m:[38;2;72;103;168m:[38;2;63;77;115m'[38;2;0;0;0m                              [0m\n"
            "[0m[38;2;0;0;0m      [38;2;33;36;52m.[38;2;95;121;187mc[38;2;47;65;105m'[38;2;1;1;1m [38;2;61;76;116m,[38;2;127;131;165mo[38;2;155;144;157md[38;2;135;130;152mo[38;2;121;119;143ml[38;2;129;125;144ml[38;2;119;116;131mc[38;2;123;120;130ml[38;2;208;205;208mK[38;2;238;234;237mN[38;2;244;241;243mW[38;2;244;242;243mW[38;2;211;202;192mK[38;2;240;227;168mX[38;2;242;231;166mN[38;2;242;231;166mN[38;2;242;231;166mN[38;2;241;230;166mX[38;2;225;209;150mK[38;2;238;225;163mX[38;2;241;230;165mX[38;2;241;230;165mX[38;2;232;217;167mX[38;2;222;212;202mK[38;2;188;187;204mO[38;2;76;103;163m:[38;2;63;90;154m;[38;2;72;83;126m;[38;2;24;24;24m [38;2;0;0;0m                             [0m\n"
        "[0m[38;2;0;0;0m      [38;2;51;58;85m.[38;2;82;110;179m:[38;2;44;51;71m.[38;2;1;1;1m [38;2;102;100;113m;[38;2;230;210;196mK[38;2;237;205;187mK[38;2;230;198;185mK[38;2;130;122;137ml[38;2;94;93;124m;[38;2;99;103;137m:[38;2;104;105;134m:[38;2;245;241;241mW[38;2;245;243;242mW[38;2;242;239;243mW[38;2;243;240;243mW[38;2;231;225;222mN[38;2;216;202;159m0[38;2;242;231;166mNNN[38;2;200;180;134mO[38;2;153;124;87ml[38;2;189;166;118mx[38;2;241;230;165mX[38;2;240;228;163mX[38;2;204;187;148mO[38;2;243;237;233mW[38;2;213;212;224mK[38;2;74;95;152m;[38;2;78;92;145m;[38;2;85;81;99m,[38;2;9;9;8m [38;2;0;0;0m                             [0m\n");

    return 0;
}
