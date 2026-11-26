#include "mii_face.h"

#ifdef RECOMP_FFL_TRIAL
#include <coreinit/cache.h>
#include <coreinit/dynload.h>
#include <gx2/clear.h>
#include <gx2/context.h>
#include <gx2/draw.h>
#include <gx2/event.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/sampler.h>
#include <gx2/shaders.h>
#include <malloc.h>
#include <nn/save/save.h>
#include <whb/gfx.h>

#include <nn/ffl/FFLCharModel.h>
#include <nn/ffl/FFLCharModelDesc.h>
#include <nn/ffl/FFLCharModelSource.h>
#include <nn/ffl/FFLDrawParam.h>
#include <nn/ffl/FFLGlobal.h>
#include <nn/ffl/FFLInitDesc.h>
#include <nn/ffl/FFLModelFlag.h>
#include <nn/ffl/FFLResourceDesc.h>
#include <nn/ffl/FFLShaderCallback.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "mii_face_program.inc"
#include "rt.h"

extern "C" void FFL3DiiSetContext(GX2ContextState *context);

namespace wiiu::mii_face {
namespace {
constexpr u32 side = 128;
constexpr int tiles = 10;
struct Tile {
    GX2ColorBuffer target{};
    GX2Texture sample{};
    bool valid = false;
};
struct alignas(256) Draw {
    alignas(256) u32 projection[16];
    alignas(256) u32 material[16];
};
Tile faces[tiles];
GX2DepthBuffer depth;
GX2ContextState *context;
WHBGfxShaderGroup shaders;
GX2Sampler sampler;
FFLResourceDesc resources;
bool initialized, attempted;
bool reported;
u32 layout_shown;
int ready;
rio::BaseMtx44f projection;
Draw draws[96];
size_t draw_count;
alignas(64) float zero_uv[2];

u32 gpu_float(float value) {
    u32 bits;
    memcpy(&bits, &value, sizeof bits);
    return __builtin_bswap32(bits);
}

bool load_archive(decltype(&SAVEGetSharedDataTitlePath) path, FFLResourceType type, const char *file) {
    char name[256] = "fs:";
    if (path(0x0005001b10056000ull, file, name + 3, sizeof name - 3) != SAVE_STATUS_OK || name[3] != '/') return false;
    FILE *in = fopen(name, "rb");
    if (!in) return false;
    long size = fseek(in, 0, SEEK_END) == 0 ? ftell(in) : -1;
    void *data = size > 0 && size <= 32 * 1024 * 1024 ? memalign(0x2000, size) : nullptr;
    const bool read = data && fseek(in, 0, SEEK_SET) == 0 && fread(data, 1, size, in) == (size_t)size;
    fclose(in);
    if (!read) {
        free(data);
        return false;
    }
    resources.pData[type] = data;
    resources.size[type] = (u32)size;
    return true;
}

bool load_archives() {
    OSDynLoad_Module save;
    if (OSDynLoad_Acquire("nn_save.rpl", &save) != OS_DYNLOAD_OK) return false;
    decltype(&SAVEGetSharedDataTitlePath) path = nullptr;
    OSDynLoad_FindExport(save, OS_DYNLOAD_EXPORT_FUNC, "SAVEGetSharedDataTitlePath", (void **)&path);
    const bool ok = path && load_archive(path, FFL_RESOURCE_TYPE_MIDDLE, "FFLResMiddle.dat") &&
                    load_archive(path, FFL_RESOURCE_TYPE_HIGH, "FFLResHigh.dat");
    OSDynLoad_Release(save);
    return ok;
}

bool allocate_surface(GX2Surface &surface, GX2SurfaceFormat format, GX2SurfaceUse use,
                      GX2TileMode mode = GX2_TILE_MODE_DEFAULT) {
    surface.dim = GX2_SURFACE_DIM_TEXTURE_2D;
    surface.width = surface.height = side;
    surface.depth = surface.mipLevels = 1;
    surface.format = format;
    surface.aa = GX2_AA_MODE1X;
    surface.use = use;
    surface.tileMode = mode;
    GX2CalcSurfaceSizeAndAlignment(&surface);
    surface.image = memalign(surface.alignment, surface.imageSize);
    return surface.image != nullptr;
}

bool initialize() {
    if (attempted) return initialized;
    attempted = true;
    if (!load_archives()) {
        LOG("[mii faces] shared face resources unavailable\n");
        release();
        attempted = true;
        return false;
    }
    context = (GX2ContextState *)memalign(GX2_CONTEXT_STATE_ALIGNMENT, sizeof *context);
    if (!context || !WHBGfxLoadGFDShaderGroup(&shaders, 0, mii_face_program) ||
        !WHBGfxInitShaderAttribute(&shaders, "aPos", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32) ||
        !WHBGfxInitShaderAttribute(&shaders, "aUV", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32) ||
        !WHBGfxInitFetchShader(&shaders) ||
        !allocate_surface(depth.surface, GX2_SURFACE_FORMAT_FLOAT_R32, GX2_SURFACE_USE_DEPTH_BUFFER)) {
        LOG("[mii faces] cannot allocate portrait render state\n");
        release();
        attempted = true;
        return false;
    }
    depth.viewNumSlices = 1;
    GX2InitDepthBufferRegs(&depth);
    GX2InitSampler(&sampler, GX2_TEX_CLAMP_MODE_CLAMP, GX2_TEX_XY_FILTER_MODE_LINEAR);
    GX2Invalidate(GX2_INVALIDATE_MODE_CPU_ATTRIBUTE_BUFFER, zero_uv, sizeof zero_uv);
    FFLInitDesc configuration{};
    configuration.fontRegion = FFL_FONT_REGION_JP_US_EU;
    const FFLResult result = FFLInitResEx(&configuration, &resources);
    if (result != FFL_RESULT_OK) {
        LOG("[mii faces] FFL resource setup failed (%u)\n", (u32)result);
        release();
        attempted = true;
        return false;
    }
    GX2SetupContextStateEx(context, TRUE);
    GX2SetContextState(context);
    FFL3DiiSetContext(context);
    FFLInitResGPUStep();
    initialized = true;
    LOG("[mii faces] FFL ready\n");
    return true;
}

void set_matrix(void *, const rio::BaseMtx44f *matrix) { projection = *matrix; }

void alpha_test(void *, bool enabled, rio::Graphics::CompareFunc compare, float reference) {
    GX2SetAlphaTest(enabled, (GX2CompareFunction)compare, reference);
}

void draw(void *, const FFLDrawParam *param) {
    const auto &attrs = param->attributeBufferParam.attributeBuffers;
    const auto &vertices = attrs[FFL_ATTRIBUTE_BUFFER_TYPE_POSITION];
    const auto &uv = attrs[FFL_ATTRIBUTE_BUFFER_TYPE_TEXCOORD];
    const auto &elements = param->primitiveParam;
    if (!vertices.ptr || !elements.pIndexBuffer || !elements.indexCount || draw_count == std::size(draws)) return;
    const auto &mat = param->modulateParam;
    const bool textured = mat.mode != FFL_MODULATE_MODE_CONSTANT;
    const auto *tex = textured && mat.pTexture2D && mat.pTexture2D != FFL_TEXTURE_PLACEHOLDER ?
                      mat.pTexture2D->getNativeTextureHandle() : nullptr;
    if (textured && (!tex || !tex->surface.image)) return;
    if (tex) {
        GX2Invalidate(GX2_INVALIDATE_MODE_TEXTURE, tex->surface.image, tex->surface.imageSize);
        GX2SetPixelTexture(tex, 0);
        GX2SetPixelSampler(&sampler, 0);
    }
    Draw &block = draws[draw_count++];
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            block.projection[col * 4 + row] = gpu_float(projection.m[row][col]);
    const FFLColor *channels[] = {mat.pColorR, mat.pColorG, mat.pColorB};
    for (int i = 0; i < 3; ++i) {
        const FFLColor tint = channels[i] ? *channels[i] : FFLColor{1, 1, 1, 1};
        block.material[i * 4] = gpu_float(tint.r);
        block.material[i * 4 + 1] = gpu_float(tint.g);
        block.material[i * 4 + 2] = gpu_float(tint.b);
        block.material[i * 4 + 3] = gpu_float(tint.a);
    }
    block.material[12] = gpu_float((float)mat.mode);
    GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_CPU | GX2_INVALIDATE_MODE_UNIFORM_BLOCK), &block, sizeof block);
    GX2SetShaderMode(GX2_SHADER_MODE_UNIFORM_BLOCK);
    GX2SetFetchShader(&shaders.fetchShader);
    GX2SetVertexShader(shaders.vertexShader);
    GX2SetPixelShader(shaders.pixelShader);
    GX2SetVertexUniformBlock(0, sizeof block.projection, block.projection);
    GX2SetPixelUniformBlock(0, sizeof block.material, block.material);
    GX2SetCullOnlyControl(GX2_FRONT_FACE_CCW, param->cullMode == FFL_CULL_MODE_FRONT, param->cullMode == FFL_CULL_MODE_BACK);
    GX2SetAttribBuffer(0, vertices.size, vertices.stride, vertices.ptr);
    GX2SetAttribBuffer(1, uv.ptr ? uv.size : sizeof zero_uv, uv.ptr ? uv.stride : 0, uv.ptr ? uv.ptr : zero_uv);
    GX2DrawIndexedEx((GX2PrimitiveMode)elements.primitiveType, elements.indexCount, GX2_INDEX_TYPE_U16,
                     elements.pIndexBuffer, 0, 1);
}

bool frame_model(const FFLCharModel &model) {
    float lo[] = {INFINITY, INFINITY, INFINITY};
    float hi[] = {-INFINITY, -INFINITY, -INFINITY};
    for (const auto get : {FFLGetDrawParamOpaFaceline, FFLGetDrawParamOpaHair, FFLGetDrawParamOpaNose,
                           FFLGetDrawParamOpaForehead, FFLGetDrawParamOpaBeard}) {
        const auto *draw_param = get(&model);
        if (!draw_param) continue;
        const auto &pos = draw_param->attributeBufferParam.attributeBuffers[FFL_ATTRIBUTE_BUFFER_TYPE_POSITION];
        if (!pos.ptr || pos.stride < 3 * sizeof(float)) continue;
        for (u32 offset = 0; offset + pos.stride <= pos.size; offset += pos.stride) {
            float xyz[3];
            memcpy(xyz, (const u8 *)pos.ptr + offset, sizeof xyz);
            for (int axis = 0; axis < 3; ++axis) {
                lo[axis] = std::min(lo[axis], xyz[axis]);
                hi[axis] = std::max(hi[axis], xyz[axis]);
            }
        }
    }
    const float span = std::max(hi[0] - lo[0], hi[1] - lo[1]);
    if (!std::isfinite(span) || span <= 0 || hi[2] <= lo[2]) return false;
    projection = {};
    const float scale = 1.78f / span;
    for (int axis = 0; axis < 2; ++axis) {
        projection.m[axis][axis] = scale;
        projection.m[axis][3] = -scale * (hi[axis] + lo[axis]) / 2;
    }
    projection.m[2][2] = -0.8f / (hi[2] - lo[2]);
    projection.m[2][3] = 0.1f - hi[2] * projection.m[2][2];
    projection.m[3][3] = 1;
    return true;
}

bool visible_pixels(GX2Surface &source) {
    GX2Surface linear{};
    if (!allocate_surface(linear, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, GX2_SURFACE_USE_TEXTURE, GX2_TILE_MODE_LINEAR_ALIGNED))
        return false;
    memset(linear.image, 0, linear.imageSize);
    DCFlushRange(linear.image, linear.imageSize);
    GX2CopySurface(&source, 0, 0, &linear, 0, 0);
    GX2DrawDone();
    DCInvalidateRange(linear.image, linear.imageSize);
    const auto *pixels = (const u32 *)linear.image;
    u32 opaque = 0;
    for (u32 y = 0; y < side; ++y)
        for (u32 x = 0; x < side; ++x)
            opaque += (pixels[y * linear.pitch + x] & 0xff) >= 128;
    free(linear.image);
    return opaque > side;
}

bool render(Tile &tile, const mii_select::Request::Entry &entry) {
    if (!tile.target.surface.image) {
        const auto use = GX2_SURFACE_USE_TEXTURE | GX2_SURFACE_USE_COLOR_BUFFER;
        if (!allocate_surface(tile.target.surface, GX2_SURFACE_FORMAT_UNORM_R8_G8_B8_A8, use)) return false;
        tile.target.viewNumSlices = 1;
        GX2InitColorBufferRegs(&tile.target);
        tile.sample.surface = tile.target.surface;
        tile.sample.viewNumMips = tile.sample.viewNumSlices = 1;
        tile.sample.compMap = 0x00010203;
        GX2InitTextureRegs(&tile.sample);
    }
    FFLCharModel model{};
    FFLCharModelDesc desc{};
    desc.resolution = (FFLResolution)side;
    desc.expressionFlag = 1u << FFL_EXPRESSION_NORMAL;
    desc.modelFlag = FFL_MODEL_FLAG_NORMAL;
    desc.resourceType = FFL_RESOURCE_TYPE_MIDDLE;
    const FFLCharModelSource source{FFL_DATA_SOURCE_STORE_DATA, entry.store_data.data(), 0};
    const FFLResult result = FFLInitCharModelCPUStep(&model, &source, &desc);
    if (result != FFL_RESULT_OK) {
        LOG("[mii faces] model setup failed (%u)\n", (u32)result);
        return false;
    }
    draw_count = 0;
    GX2SetContextState(context);
    FFLShaderCallback callback{};
    callback.pSetMatrixFunc = set_matrix;
    callback.pDrawFunc = draw;
    callback.pApplyAlphaTestFunc = alpha_test;
    FFLInitCharModelGPUStepWithCallback(&model, &callback);
    const bool framed = frame_model(model);
    const size_t model_start = draw_count;
    if (framed) {
        GX2ClearColor(&tile.target, 0, 0, 0, 0);
        GX2ClearDepthStencilEx(&depth, 1, 0, GX2_CLEAR_FLAGS_DEPTH);
        GX2SetContextState(context);
        GX2SetViewport(0, 0, side, side, 0, 1);
        GX2SetScissor(0, 0, side, side);
        GX2SetColorBuffer(&tile.target, GX2_RENDER_TARGET_0);
        GX2SetDepthBuffer(&depth);
        GX2SetTargetChannelMasks(GX2_CHANNEL_MASK_RGBA, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0,
                                (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0, (GX2ChannelMask)0);
        GX2SetColorControl(GX2_LOGIC_OP_COPY, 0, FALSE, TRUE);
        GX2SetAlphaTest(FALSE, GX2_COMPARE_FUNC_ALWAYS, 0);
        GX2SetDepthOnlyControl(TRUE, TRUE, GX2_COMPARE_FUNC_LEQUAL);
        FFLDrawOpaWithCallback(&model, &callback);
        GX2SetColorControl(GX2_LOGIC_OP_COPY, 1, FALSE, TRUE);
        GX2SetBlendControl(GX2_RENDER_TARGET_0, GX2_BLEND_MODE_SRC_ALPHA, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD,
                          TRUE, GX2_BLEND_MODE_ONE, GX2_BLEND_MODE_INV_SRC_ALPHA, GX2_BLEND_COMBINE_MODE_ADD);
        GX2SetDepthOnlyControl(TRUE, FALSE, GX2_COMPARE_FUNC_LEQUAL);
        FFLDrawXluWithCallback(&model, &callback);
        GX2Invalidate((GX2InvalidateMode)(GX2_INVALIDATE_MODE_COLOR_BUFFER | GX2_INVALIDATE_MODE_TEXTURE),
                      tile.target.surface.image, tile.target.surface.imageSize);
    }
    GX2DrawDone();
    FFLDeleteCharModel(&model);
    const bool valid = framed && model_start != draw_count;
    if (!valid || (!reported && !visible_pixels(tile.target.surface))) {
        LOG("[mii faces] model produced no visible portrait\n");
        return false;
    }
    return true;
}
}  // namespace

bool prepare(u32 layout, const mii_select::Request::Entry *const *entries, int count) {
    if (!initialize()) return false;
    if (layout != layout_shown) {
        layout_shown = layout;
        ready = 0;
    }
    if (ready < std::min(tiles, count)) {
        faces[ready].valid = render(faces[ready], *entries[ready]);
        if (faces[ready].valid && !reported) {
            reported = true;
            LOG("[mii faces] first thumbnail rendered\n");
        }
        ++ready;
        return true;
    }
    return false;
}

const GX2Texture *texture(int cell) {
    return cell < ready && faces[cell].valid ? &faces[cell].sample : nullptr;
}

void release() {
    if (!attempted) return;
    if (initialized) FFLExit();
    for (auto &face : faces) {
        free(face.target.surface.image);
        face = {};
    }
    free(depth.surface.image);
    depth = {};
    FFL3DiiSetContext(nullptr);
    free(context);
    context = nullptr;
    if (shaders.vertexShader || shaders.pixelShader || shaders.fetchShaderProgram) WHBGfxFreeShaderGroup(&shaders);
    shaders = {};
    for (const auto *data : resources.pData) free((void *)data);
    resources = {};
    attempted = initialized = reported = false;
    layout_shown = 0;
    ready = 0;
}
}  // namespace wiiu::mii_face
#endif
