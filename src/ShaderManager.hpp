#pragma once

#include <GLES3/gl32.h>
#include <hyprland/src/render/Shader.hpp>
#include <string>
#include <array>
#include <span>

struct SCleanSampleUniforms {
    GLint texture = -1;
    GLint origin = -1;
    GLint ownerBounds = -1;
    void locate(GLuint program);
};

struct SGlassUniforms {
#define GLASS_PARAM(name, value, lo, hi) GLint name = -1;
#include "MaterialParams.inc"
#undef GLASS_PARAM
    GLint uvPadding = -1;
    GLint blurTexture = -1;
    GLint pixelScale = -1;
    GLint fadeAlpha = -1;
    SCleanSampleUniforms clean;
    GLint cleanBlurTexture = -1;
    GLint hasCleanBlur = -1;
    GLint useNeighborContribution = -1;
    GLint underlayShapes = -1;
    GLint underlayCount = -1;

    // Layers only: temp FBO surface mask for content-aware glass
    GLint maskTex = -1;
    GLint useMask = -1;
    GLint maskUVOffset = -1;
    GLint maskUVScale = -1;
    GLint maskAlphaThreshold = -1;
    GLint maskMode = -1;
    GLint regionRectCount = -1;
    GLint regionRects = -1;
};

struct SBlurUniforms {
    GLint direction = -1;
    GLint filterMode = -1;
    GLint cleanInput = -1;
    SCleanSampleUniforms clean;
};

struct SSceneSampleUniforms {
    GLint sceneTex = -1;
    GLint backgroundTex = -1;
    GLint sampleOrigin = -1;
    GLint ownerBounds = -1;
    GLint neighborContribution = -1;
};

class CShaderManager {
  public:
    [[nodiscard]] bool isInitialized() const noexcept { return m_initialized; }

    void initializeIfNeeded();
    void destroy() noexcept;
    // Upload on the caller's active texture unit; storage is reused between draws.
    void bindUnderlayShapes(std::span<const std::array<float, 8>> shapes);

    SP<CShader>    glassShader = makeShared<CShader>();
    SGlassUniforms glassUniforms;

    SP<CShader>    blurShader = makeShared<CShader>();
    SBlurUniforms  blurUniforms;

    SP<CShader> sceneSampleShader = makeShared<CShader>();
    SSceneSampleUniforms sceneSampleUniforms;

  private:
    bool m_initialized = false;
    GLuint m_underlayShapes = 0;
    size_t m_underlayCapacity = 0;

    [[nodiscard]] static std::string loadShaderSource(const char* fileName);
    [[nodiscard]] bool compileGlassShader();
    [[nodiscard]] bool compileBlurShader();
    [[nodiscard]] bool compileSceneSampleShader();
};
