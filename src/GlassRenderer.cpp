#include "GlassRenderer.hpp"
#include "Globals.hpp"
#include "RenderSafety.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <GLES3/gl32.h>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/utils/ScopeGuard.hpp>

namespace GlassRenderer {

static GLuint fbId(const SP<Render::IFramebuffer>& framebuffer) {
    return dynamic_cast<Render::GL::CGLFramebuffer*>(framebuffer.get())->getFBID();
}

static void bindTexture(const SP<Render::IFramebuffer>& framebuffer, GLenum unit) {
    glActiveTexture(unit);
    framebuffer->getTexture()->bind();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static void bindCleanBackdrop(const SCleanSampleUniforms& uniforms, const SNeighborBackdrop& clean, GLenum unit) {
    g_pGlobalState->renderBuffers.touch(clean.background);
    bindTexture(clean.background, unit);
    glUniform1i(uniforms.texture, unit - GL_TEXTURE0);
    glUniform2iv(uniforms.origin, 1, clean.sampleOrigin.data());
    glUniform4iv(uniforms.ownerBounds, 1, clean.ownerBounds.data());
}

bool sampleBackground(SP<Render::IFramebuffer>& sampleFramebuffer, SP<Render::IFramebuffer> sourceFramebuffer,
                      CBox box, Vector2D& outPaddingRatio, int padding, SP<Render::IFramebuffer> neighbours,
                      SP<Render::IFramebuffer> sceneBackground, float neighborContribution) {
    const auto geometry = RenderSafety::sampleGeometry(box.x, box.y, box.width, box.height, padding);
    if (!sourceFramebuffer || !sourceFramebuffer->isAllocated() || !geometry ||
        !RenderSafety::dimension(sourceFramebuffer->m_size.x) || !RenderSafety::dimension(sourceFramebuffer->m_size.y))
        return false;

    const auto [width, height, x0, y0, x1, y1, owner] = *geometry;
    const int fw = static_cast<int>(sourceFramebuffer->m_size.x);
    const int fh = static_cast<int>(sourceFramebuffer->m_size.y);
    const int sx0 = std::clamp(x0, 0, fw), sy0 = std::clamp(y0, 0, fh);
    const int sx1 = std::clamp(x1, 0, fw), sy1 = std::clamp(y1, 0, fh);
    if (sx1 <= sx0 || sy1 <= sy0)
        return false;
    const int dx0 = sx0 - x0, dy0 = sy0 - y0;
    const int dx1 = sx1 - x0, dy1 = sy1 - y0;

    Hyprutils::Utils::CScopeGuard restore([&] { glBindFramebuffer(GL_FRAMEBUFFER, fbId(sourceFramebuffer)); });
    if (!g_pGlobalState->renderBuffers.prepare(sampleFramebuffer, width, height, sourceFramebuffer->m_drmFormat, "sharp"))
        return false;
    g_pGlobalState->renderBuffers.touch(sourceFramebuffer);
    g_pGlobalState->renderBuffers.touch(neighbours);

    outPaddingRatio = Vector2D(static_cast<double>(padding) / width, static_cast<double>(padding) / height);
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(sampleFramebuffer));
    const bool useNeighbours = neighbours && neighbours->isAllocated() && neighbours->m_size == sourceFramebuffer->m_size &&
                               neighbours->m_drmFormat == sourceFramebuffer->m_drmFormat;
    const auto [ownX0, ownY0, ownX1, ownY1] = owner;
    if (useNeighbours && neighborContribution < 1.0f && sceneBackground &&
        sceneBackground->isAllocated() && sceneBackground->m_size == sourceFramebuffer->m_size && sceneBackground->m_drmFormat == sourceFramebuffer->m_drmFormat) {
        // The native source may already contain earlier-drawn neighbours.
        // Fade towards the immutable scene background outside the owner, so
        // strength zero and partial strengths cannot depend on focus/order.
        // Keep the owner's real backdrop intact; blur and glass run normally.
        g_pGlobalState->renderBuffers.touch(sceneBackground);
        GLint viewport[4];
        glGetIntegerv(GL_VIEWPORT, viewport);
        const bool blend = glIsEnabled(GL_BLEND);
        g_pHyprOpenGL->setCapStatus(GL_BLEND, false);
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(sampleFramebuffer));
        g_pHyprOpenGL->setViewport(0, 0, width, height);
        static constexpr std::array<float, 9> projection{2, 0, 0, 0, 2, 0, -1, -1, 1};
        auto& manager = g_pGlobalState->shaderManager;
        auto shader = g_pHyprOpenGL->useShader(manager.sceneSampleShader);
        shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, projection);
        shader->setUniformInt(SHADER_TEX, 0);
        glUniform1i(manager.sceneSampleUniforms.sceneTex, 1);
        glUniform1i(manager.sceneSampleUniforms.backgroundTex, 2);
        glUniform2i(manager.sceneSampleUniforms.sampleOrigin, x0, y0);
        glUniform4i(manager.sceneSampleUniforms.ownerBounds, ownX0, ownY0, ownX1, ownY1);
        glUniform1f(manager.sceneSampleUniforms.neighborContribution, std::clamp(neighborContribution, 0.0f, 1.0f));
        bindTexture(sourceFramebuffer, GL_TEXTURE0);
        bindTexture(neighbours, GL_TEXTURE1);
        bindTexture(sceneBackground, GL_TEXTURE2);
        glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
        g_pHyprOpenGL->setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        g_pHyprOpenGL->setCapStatus(GL_BLEND, blend);
        return true;
    }
    auto copy = [&](int ax, int ay, int bx, int by, int cx, int cy, int dx, int dy) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(sourceFramebuffer));
        glBlitFramebuffer(ax, ay, bx, by, cx, cy, dx, dy, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        if (!useNeighbours)
            return;
        const int left = std::clamp(ownX0, ax, bx), right = std::clamp(ownX1, ax, bx);
        const int top = std::clamp(ownY0, ay, by), bottom = std::clamp(ownY1, ay, by);
        // Four nonoverlapping rectangles exclude the entire owner, including
        // rounded corners. Apply the same composition to offscreen gutters.
        const std::array<std::array<int, 4>, 4> regions{{
            {ax, ay, bx, top}, {ax, bottom, bx, by},
            {ax, top, left, bottom}, {right, top, bx, bottom},
        }};
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbId(neighbours));
        for (const auto& r : regions) {
            if (r[0] >= r[2] || r[1] >= r[3])
                continue;
            auto tx = [&](int x) { return RenderSafety::mapBlitCoordinate(x, ax, bx, cx, dx); };
            auto ty = [&](int y) { return RenderSafety::mapBlitCoordinate(y, ay, by, cy, dy); };
            const auto mappedX0 = tx(r[0]), mappedY0 = ty(r[1]), mappedX1 = tx(r[2]), mappedY1 = ty(r[3]);
            if (!mappedX0 || !mappedY0 || !mappedX1 || !mappedY1)
                continue;
            glBlitFramebuffer(r[0], r[1], r[2], r[3], *mappedX0, *mappedY0, *mappedX1, *mappedY1, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        }
    };
    copy(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1);

    // Extrude the monitor's edge pixels into offscreen padding. Clearing these
    // gutters to black darkens the blur when a window touches a screen edge.
    const std::array<int, 4> sx{sx0, sx0, sx1, sx1};
    const std::array<int, 4> sy{sy0, sy0, sy1, sy1};
    const std::array<int, 4> dx{0, dx0, dx1, width};
    const std::array<int, 4> dy{0, dy0, dy1, height};
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 3; ++col) {
            if ((row == 1 && col == 1) || dx[col] == dx[col + 1] || dy[row] == dy[row + 1])
                continue;
            const int ax = col == 2 ? sx1 - 1 : sx[col];
            const int bx = col == 0 ? sx0 + 1 : sx[col + 1];
            const int ay = row == 2 ? sy1 - 1 : sy[row];
            const int by = row == 0 ? sy0 + 1 : sy[row + 1];
            copy(ax, ay, bx, by, dx[col], dy[row], dx[col + 1], dy[row + 1]);
        }
    }
    return true;
}

bool prepareMaterialBackdrop(SP<Render::IFramebuffer> sample, SP<Render::IFramebuffer>& halfSharp,
                             SP<Render::IFramebuffer> caller, const SNeighborBackdrop* composition, float contribution) {
    if (!caller || !sample || !sample->isAllocated() || sample == halfSharp ||
        !RenderSafety::dimension(sample->m_size.x) || !RenderSafety::dimension(sample->m_size.y)) return false;
    GLint viewport[4]; glGetIntegerv(GL_VIEWPORT, viewport);
    GLint scissorBox[4]; glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    const bool blend = glIsEnabled(GL_BLEND), scissor = glIsEnabled(GL_SCISSOR_TEST);
    Hyprutils::Utils::CScopeGuard restore([&] {
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(caller));
        g_pHyprOpenGL->setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        g_pHyprOpenGL->setCapStatus(GL_BLEND, blend);
        glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
        g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, scissor);
        glActiveTexture(GL_TEXTURE0);
    });
    const int width = (static_cast<int>(sample->m_size.x) + 1) / 2;
    const int height = (static_cast<int>(sample->m_size.y) + 1) / 2;
    if (!g_pGlobalState->renderBuffers.prepare(halfSharp, width, height, sample->m_drmFormat, "material-backdrop")) return false;
    halfSharp->setImageDescription(sample->imageDescription());
    auto& manager = g_pGlobalState->shaderManager;
    const auto& u = manager.materialBackdropUniforms;
    auto shader = g_pHyprOpenGL->useShader(manager.materialBackdropShader);
    static constexpr std::array<float, 9> projection{2, 0, 0, 0, 2, 0, -1, -1, 1};
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, projection);
    shader->setUniformInt(SHADER_TEX, 0);
    const bool scene = composition && composition->scene && composition->background;
    glUniform1i(u.useScene, scene);
    glUniform1i(u.sceneTex, 1); glUniform1i(u.backgroundTex, 2);
    if (scene) {
        bindTexture(composition->scene, GL_TEXTURE1); bindTexture(composition->background, GL_TEXTURE2);
        g_pGlobalState->renderBuffers.touch(composition->scene);
        g_pGlobalState->renderBuffers.touch(composition->background);
        glUniform2iv(u.sampleOrigin, 1, composition->sampleOrigin.data());
        glUniform4iv(u.ownerBounds, 1, composition->ownerBounds.data());
        glUniform1f(u.neighborContribution, std::clamp(contribution, 0.0f, 1.0f));
    } else {
        bindTexture(sample, GL_TEXTURE1); bindTexture(sample, GL_TEXTURE2);
    }
    bindTexture(sample, GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, fbId(halfSharp));
    g_pHyprOpenGL->setViewport(0, 0, width, height);
    g_pHyprOpenGL->setCapStatus(GL_BLEND, false);
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    return true;
}

bool blurBackground(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer>& blurredFramebuffer,
                    float sigma, SP<Render::IFramebuffer> callerFramebuffer, const SNeighborBackdrop* clean,
                    SP<Render::IFramebuffer> materialBackdrop) {
    if (!callerFramebuffer)
        return false;
    GLint viewport[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    Hyprutils::Utils::CScopeGuard restore([&] {
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(callerFramebuffer));
        g_pHyprOpenGL->setViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    });
    if (!sampleFramebuffer || !sampleFramebuffer->isAllocated() || !std::isfinite(sigma) ||
        !RenderSafety::dimension(sampleFramebuffer->m_size.x) || !RenderSafety::dimension(sampleFramebuffer->m_size.y))
        return false;
    if (sigma <= 0.0f) {
        blurredFramebuffer.reset();
        return true;
    }
    const int sourceWidth = static_cast<int>(sampleFramebuffer->m_size.x);
    const int sourceHeight = static_cast<int>(sampleFramebuffer->m_size.y);
    int width = std::max(1, (sourceWidth + 1) / 2);
    int height = std::max(1, (sourceHeight + 1) / 2);
    if (materialBackdrop && (!materialBackdrop->isAllocated() || materialBackdrop->m_size != Vector2D(width, height) ||
                            materialBackdrop == blurredFramebuffer || materialBackdrop->m_drmFormat != sampleFramebuffer->m_drmFormat))
        return false;
    auto allocate = [&](SP<Render::IFramebuffer>& framebuffer, int w, int h, const char* role) {
        return g_pGlobalState->renderBuffers.prepare(framebuffer, w, h, sampleFramebuffer->m_drmFormat, role);
    };
    if (!allocate(blurredFramebuffer, width, height, "blurred"))
        return false;

    // Preserve the half-resolution result, but prefilter into smaller levels
    // until the Gaussian's taps overlap. Merely stretching the fixed kernel
    // across the sharp image leaves a visible grid at larger effect scales.
    // Each level halves a positive int dimension (rounding up), starting at
    // half resolution. Even INT_MAX therefore needs at most int::digits
    // levels to reach 1x1. Keep ownership local to this invocation, including
    // auxiliary passes, without a per-blur heap allocation or an arbitrary cap.
    std::array<SP<Render::IFramebuffer>, std::numeric_limits<int>::digits> levelStorage;
    size_t levelCount = 1;
    levelStorage[0] = blurredFramebuffer;
    auto& pyramid = g_pGlobalState->blurPyramid;
    while ((sigma * width / sourceWidth > 2.8f || sigma * height / sourceHeight > 2.8f) &&
           (width > 1 || height > 1)) {
        width = std::max(1, (width + 1) / 2);
        height = std::max(1, (height + 1) / 2);
        const size_t index = levelCount - 1;
        if (pyramid.size() <= index)
            pyramid.emplace_back();
        if (!allocate(pyramid[index], width, height, "blur-pyramid"))
            return false;
        levelStorage[levelCount++] = pyramid[index];
    }
    const std::span levels{levelStorage.data(), levelCount};
    auto& temporary = g_pGlobalState->blurTempFramebuffer;
    if (!allocate(temporary, width, height, "blur-temp"))
        return false;

    static constexpr std::array<float, 9> projection{2, 0, 0, 0, 2, 0, -1, -1, 1};
    auto& manager = g_pGlobalState->shaderManager;
    auto shader = g_pHyprOpenGL->useShader(manager.blurShader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, projection);
    shader->setUniformInt(SHADER_TEX, 0);
    glUniform1i(manager.blurUniforms.cleanInput, clean ? 1 : 0);
    if (clean)
        bindCleanBackdrop(manager.blurUniforms.clean, *clean, GL_TEXTURE1);
    else
        glUniform1i(manager.blurUniforms.clean.texture, 0);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    auto draw = [&](const SP<Render::IFramebuffer>& target, const SP<Render::IFramebuffer>& input) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(target));
        g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(target->m_size.x), static_cast<int>(target->m_size.y));
        bindTexture(input, GL_TEXTURE0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };

    glUniform1i(manager.blurUniforms.filterMode, 1);
    SP<Render::IFramebuffer> input = materialBackdrop ? materialBackdrop : sampleFramebuffer;
    for (const auto& level : levels) {
        // Copy the retained half-sharp input before filtering; never overwrite
        // it or apply a second reduction at the same texture density.
        glUniform1i(manager.blurUniforms.filterMode, materialBackdrop && level == levels.front() ? 3 : 1);
        draw(level, input);
        level->setImageDescription(sampleFramebuffer->imageDescription());
        // Later levels already contain the clean image. Only the first
        // reduction reconstructs it, avoiding a full-resolution copy/target.
        glUniform1i(manager.blurUniforms.cleanInput, 0);
        input = level;
    }

    // The down/up filters already contribute some blur. Subtract their
    // approximate variance to keep the requested radius stable across levels.
    const float scaleX = static_cast<float>(sourceWidth) / width;
    const float scaleY = static_cast<float>(sourceHeight) / height;
    const float baseX = sourceWidth / blurredFramebuffer->m_size.x;
    const float baseY = sourceHeight / blurredFramebuffer->m_size.y;
    const float sigmaX = std::sqrt(std::max(0.0f, sigma * sigma - 0.4f * (scaleX * scaleX - baseX * baseX)));
    const float sigmaY = std::sqrt(std::max(0.0f, sigma * sigma - 0.4f * (scaleY * scaleY - baseY * baseY)));
    glUniform1i(manager.blurUniforms.filterMode, 0);
    for (int iteration = 0; iteration < 2; ++iteration) {
        glUniform2f(manager.blurUniforms.direction, sigmaX / (2.8f * sourceWidth), 0.0f);
        draw(temporary, input);
        glUniform2f(manager.blurUniforms.direction, 0.0f, sigmaY / (2.8f * sourceHeight));
        draw(input, temporary);
    }

    glUniform1i(manager.blurUniforms.filterMode, 2);
    for (size_t i = levels.size() - 1; i > 0; --i)
        draw(levels[i - 1], levels[i]);

    glBindVertexArray(0);
    return true;
}

void applyGlassEffect(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer> blurredFramebuffer,
                      SP<Render::IFramebuffer> targetFramebuffer, CBox& rawBox, CBox& transformedBox,
                      float alpha, float cornerRadius, float roundingPower,
                      const Vector2D& paddingRatio, const SMaterial& material, const SMaskInfo* mask,
                      const SNeighborBackdrop* neighbors, SP<Render::IFramebuffer> materialBackdrop, bool renderShadow) {
    if (!sampleFramebuffer || !targetFramebuffer)
        return;
    g_pGlobalState->renderBuffers.touch(sampleFramebuffer);
    g_pGlobalState->renderBuffers.touch(blurredFramebuffer);
    auto& manager = g_pGlobalState->shaderManager;
    const bool liquid = material.family == EMaterialFamily::LIQUID;
    if (liquid && !materialBackdrop) return;
    const auto& uniforms = liquid ? manager.liquidCommonUniforms : manager.glassUniforms;
    const float extent = liquid && !mask && renderShadow ? std::ceil(materialOutputExtent(material) *
        g_pHyprRenderer->m_renderData.pMonitor->m_scale * material.effect_scale) : 0.0f;
    CBox outputBox = rawBox;
    outputBox.expand(extent);
    const auto transform = Math::wlTransformToHyprutils(
        Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform));
    Mat3x3 matrix = g_pHyprRenderer->projectBoxToTarget(outputBox, transform);
    matrix.transpose();

    glBindFramebuffer(GL_FRAMEBUFFER, fbId(targetFramebuffer));
    bindTexture(sampleFramebuffer, GL_TEXTURE0);
    bindTexture(blurredFramebuffer ? blurredFramebuffer : (liquid ? materialBackdrop : sampleFramebuffer), GL_TEXTURE1);
    if (mask && mask->textureId != 0) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(mask->target, mask->textureId);
    }
    if (neighbors) {
        g_pGlobalState->renderBuffers.touch(neighbors->blurred);
        bindTexture(neighbors->blurred ? neighbors->blurred :
                    (liquid && neighbors->halfSharp ? neighbors->halfSharp : sampleFramebuffer), GL_TEXTURE4);
    }
    if (liquid) {
        bindTexture(materialBackdrop, GL_TEXTURE6);
        bindTexture(neighbors && neighbors->halfSharp ? neighbors->halfSharp : materialBackdrop, GL_TEXTURE7);
        g_pGlobalState->renderBuffers.touch(materialBackdrop);
        if (neighbors) g_pGlobalState->renderBuffers.touch(neighbors->halfSharp);
    }
    int underlayCount = 0;
    if (neighbors && !neighbors->underlayShapes.empty()) {
        glActiveTexture(GL_TEXTURE5);
        underlayCount = manager.bindUnderlayShapes(neighbors->underlayShapes);
    }
    glActiveTexture(GL_TEXTURE0);

    auto shader = g_pHyprOpenGL->useShader(liquid ? manager.liquidShader : manager.glassShader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, matrix.getMatrix());
    shader->setUniformInt(SHADER_TEX, 0);
    shader->setUniformFloat2(SHADER_FULL_SIZE, transformedBox.width, transformedBox.height);
    shader->setUniformFloat(SHADER_RADIUS, cornerRadius);
    shader->setUniformFloat(SHADER_ROUNDING_POWER, roundingPower);
    glUniform1i(uniforms.blurTexture, 1);
    if (neighbors && !liquid)
        bindCleanBackdrop(uniforms.clean, *neighbors, GL_TEXTURE3);
    glActiveTexture(GL_TEXTURE0);
    glUniform1i(uniforms.cleanBlurTexture, 4);
    glUniform1i(uniforms.hasCleanBlur, neighbors && neighbors->blurred ? 1 : 0);
    glUniform1i(uniforms.useNeighborContribution, neighbors && (!liquid || neighbors->halfSharp) ? 1 : 0);
    glUniform1i(uniforms.underlayShapes, 5);
    glUniform1i(uniforms.underlayCount, underlayCount);
    glUniform1f(uniforms.pixelScale, g_pHyprRenderer->m_renderData.pMonitor->m_scale * material.effect_scale);
    glUniform1f(uniforms.fadeAlpha, std::clamp(alpha, 0.0f, 1.0f));
    glUniform2f(uniforms.uvPadding, paddingRatio.x, paddingRatio.y);
#define GLASS_PARAM(name, value, lo, hi) glUniform1f(uniforms.name, material.name);
#include "MaterialParams.inc"
#undef GLASS_PARAM
    if (liquid) {
        const auto& u = manager.liquidUniforms;
        // Output rotation/reflection is a coordinate transform, separate from
        // material display angle. Convert through the
        // compositor's own transform, with y-up material vectors on both sides.
        const auto origin = Vector2D{0, 0}.transform(transform, {1, 1});
        const auto x = Vector2D{1, 0}.transform(transform, {1, 1}) - origin;
        const auto y = Vector2D{0, -1}.transform(transform, {1, 1}) - origin;
        u.upload(effectiveLiquidMaterial(material), material.display_angle_degrees,
                 {static_cast<float>(x.x), static_cast<float>(-x.y), static_cast<float>(y.x), static_cast<float>(-y.y)});
        glUniform1i(u.backdrop, 6); glUniform1i(u.cleanBackdrop, 7);
        glUniform2f(u.outputExtent, extent, extent);
        glUniform1i(u.renderShadow, !mask && renderShadow);
    }

    // Window falloff also applies without a neighbor scene, so the transition
    // stays consistent when windows open, close or pass underneath. Layers
    // retain their own material's existing blur ramp.
    if (mask && !liquid)
        glUniform1f(uniforms.neighbor_falloff, 0.0f);

    const bool hasMask = mask && mask->textureId != 0;
    glUniform1i(uniforms.useMask, hasMask ? 1 : 0);
    if (hasMask) {
        glUniform1i(uniforms.maskTex, 2);
        glUniform2f(uniforms.maskUVOffset, mask->uvOffset.x, mask->uvOffset.y);
        glUniform2f(uniforms.maskUVScale, mask->uvScale.x, mask->uvScale.y);
        glUniform1f(uniforms.maskAlphaThreshold, mask->alphaThreshold);
        glUniform1i(uniforms.maskMode, mask->maskMode);
        glUniform1i(uniforms.regionRectCount, mask->regionRectCount);
        if (mask->regionRectCount > 0)
            glUniform4fv(uniforms.regionRects, mask->regionRectCount,
                         reinterpret_cast<const float*>(mask->regionRects.data()));
    }
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    g_pHyprOpenGL->scissor(outputBox);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    g_pHyprOpenGL->scissor(nullptr);
}

} // namespace GlassRenderer
