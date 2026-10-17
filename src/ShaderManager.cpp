#include "ShaderManager.hpp"
#include "Globals.hpp"
#include "ShaderSources.hpp"

#include <GLES3/gl32.h>
#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/render/OpenGL.hpp>

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
    glassUniforms.cleanTexture = glGetUniformLocation(program, "clean_texture");
    glassUniforms.cleanBlurTexture = glGetUniformLocation(program, "clean_blur_texture");
    glassUniforms.useNeighborFalloff = glGetUniformLocation(program, "useNeighborFalloff");
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

void CShaderManager::destroy() noexcept {
    glassShader->destroy();
    blurShader->destroy();
    sceneSampleShader->destroy();
    m_initialized = false;
}
