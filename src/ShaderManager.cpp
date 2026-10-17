#include "ShaderManager.hpp"
#include "Globals.hpp"
#include "ShaderSources.hpp"

#include <bit>

#include <GLES3/gl32.h>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>

void SCleanSampleUniforms::locate(GLuint program) {
    texture = glGetUniformLocation(program, "clean_texture");
    origin = glGetUniformLocation(program, "cleanOrigin");
    ownerBounds = glGetUniformLocation(program, "cleanOwnerBounds");
}

std::string CShaderManager::loadShaderSource(const char* fileName) {
    if (SHADERS.contains(fileName))
        return SHADERS.at(fileName);

    const std::string message = std::format("[{}] Failed to load shader: {}", PLUGIN_NAME, fileName);
    HyprlandAPI::addNotification(PHANDLE, message, CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
    throw std::runtime_error(message);
}

bool CShaderManager::compileGlassShader() {
    if (!glassShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("glass.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile glass shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }

    const auto program = glassShader->program();

#define GLASS_PARAM(name, value, lo, hi) glassUniforms.name = glGetUniformLocation(program, #name);
#include "MaterialParams.inc"
#undef GLASS_PARAM
    glassUniforms.uvPadding = glGetUniformLocation(program, "uvPadding");
    glassUniforms.blurTexture = glGetUniformLocation(program, "blur_texture");
    glassUniforms.pixelScale = glGetUniformLocation(program, "pixelScale");
    glassUniforms.fadeAlpha = glGetUniformLocation(program, "fadeAlpha");
    glassUniforms.clean.locate(program);
    glassUniforms.cleanBlurTexture = glGetUniformLocation(program, "clean_blur_texture");
    glassUniforms.hasCleanBlur = glGetUniformLocation(program, "hasCleanBlur");
    glassUniforms.useNeighborFalloff = glGetUniformLocation(program, "useNeighborFalloff");
    glassUniforms.underlayShapes = glGetUniformLocation(program, "underlayShapes");
    glassUniforms.underlayCount = glGetUniformLocation(program, "underlayCount");
    glassUniforms.maskTex             = glGetUniformLocation(program, "maskTex");
    glassUniforms.useMask             = glGetUniformLocation(program, "useMask");
    glassUniforms.maskUVOffset        = glGetUniformLocation(program, "maskUVOffset");
    glassUniforms.maskUVScale         = glGetUniformLocation(program, "maskUVScale");
    glassUniforms.maskAlphaThreshold  = glGetUniformLocation(program, "maskAlphaThreshold");
    glassUniforms.maskMode            = glGetUniformLocation(program, "maskMode");
    glassUniforms.regionRectCount     = glGetUniformLocation(program, "regionRectCount");
    glassUniforms.regionRects         = glGetUniformLocation(program, "regionRects[0]");
    if (glassUniforms.regionRects == -1)
        glassUniforms.regionRects = glGetUniformLocation(program, "regionRects");

    return true;
}

bool CShaderManager::compileBlurShader() {
    if (!blurShader->createProgram(
            g_pHyprOpenGL->m_shaders->TEXVERTSRC,
            loadShaderSource("blur.frag"),
            true
        )) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile blur shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }

    const auto program = blurShader->program();

    blurUniforms.direction = glGetUniformLocation(program, "direction");
    blurUniforms.filterMode = glGetUniformLocation(program, "filterMode");
    blurUniforms.cleanInput = glGetUniformLocation(program, "cleanInput");
    blurUniforms.clean.locate(program);

    return true;
}

bool CShaderManager::compileSceneSampleShader() {
    if (!sceneSampleShader->createProgram(g_pHyprOpenGL->m_shaders->TEXVERTSRC,
                                          loadShaderSource("scene-sample.frag"), true)) {
        HyprlandAPI::addNotification(PHANDLE,
            std::format("[{}] Failed to compile scene sampling shader", PLUGIN_NAME),
            CHyprColor{1.0, 0.2, 0.2, 1.0}, 5000);
        return false;
    }
    const auto program = sceneSampleShader->program();
    sceneSampleUniforms.sceneTex = glGetUniformLocation(program, "sceneTex");
    sceneSampleUniforms.backgroundTex = glGetUniformLocation(program, "backgroundTex");
    sceneSampleUniforms.sampleOrigin = glGetUniformLocation(program, "sampleOrigin");
    sceneSampleUniforms.ownerBounds = glGetUniformLocation(program, "ownerBounds");
    sceneSampleUniforms.neighborContribution = glGetUniformLocation(program, "neighborContribution");
    return true;
}

void CShaderManager::initializeIfNeeded() {
    if (m_initialized)
        return;

    if (!compileGlassShader())
        return;

    if (!compileBlurShader())
        return;

    if (!compileSceneSampleShader())
        return;

    m_initialized = true;
}

void CShaderManager::bindUnderlayShapes(std::span<const std::array<float, 8>> shapes) {
    if (!m_underlayShapes)
        glGenTextures(1, &m_underlayShapes);
    glBindTexture(GL_TEXTURE_2D, m_underlayShapes);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (shapes.size() > m_underlayCapacity) {
        m_underlayCapacity = std::bit_ceil(shapes.size());
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 2, m_underlayCapacity, 0, GL_RGBA, GL_FLOAT, nullptr);
    }
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, shapes.size(), GL_RGBA, GL_FLOAT, shapes.data());
}

void CShaderManager::destroy() noexcept {
    glassShader->destroy();
    blurShader->destroy();
    sceneSampleShader->destroy();
    if (m_underlayShapes)
        glDeleteTextures(1, &m_underlayShapes);
    m_underlayShapes = 0;
    m_underlayCapacity = 0;
    m_initialized = false;
}
