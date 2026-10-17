#include "GlassRenderer.hpp"
#include "Globals.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <GLES3/gl32.h>
#include <hyprland/src/render/OpenGL.hpp>
#include <hyprland/src/render/Renderer.hpp>

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

bool sampleBackground(SP<Render::IFramebuffer>& sampleFramebuffer, SP<Render::IFramebuffer> sourceFramebuffer,
                      CBox box, Vector2D& outPaddingRatio, int padding, SP<Render::IFramebuffer> neighbours) {
    if (!sourceFramebuffer || !std::isfinite(box.x) || !std::isfinite(box.y) ||
        !std::isfinite(box.width) || !std::isfinite(box.height) || box.width <= 0 || box.height <= 0)
        return false;

    const int width = std::max(1, static_cast<int>(std::ceil(box.width))) + 2 * padding;
    const int height = std::max(1, static_cast<int>(std::ceil(box.height))) + 2 * padding;
    const int x0 = static_cast<int>(std::floor(box.x)) - padding;
    const int y0 = static_cast<int>(std::floor(box.y)) - padding;
    const int fw = static_cast<int>(sourceFramebuffer->m_size.x);
    const int fh = static_cast<int>(sourceFramebuffer->m_size.y);
    const int sx0 = std::clamp(x0, 0, fw), sy0 = std::clamp(y0, 0, fh);
    const int sx1 = std::clamp(x0 + width, 0, fw), sy1 = std::clamp(y0 + height, 0, fh);
    if (sx1 <= sx0 || sy1 <= sy0)
        return false;
    const int dx0 = sx0 - x0, dy0 = sy0 - y0;
    const int dx1 = sx1 - x0, dy1 = sy1 - y0;

    if (!sampleFramebuffer)
        sampleFramebuffer = g_pHyprRenderer->createFB("hyprliquid-sharp");
    if (sampleFramebuffer->m_size.x != width || sampleFramebuffer->m_size.y != height ||
        sampleFramebuffer->m_drmFormat != sourceFramebuffer->m_drmFormat)
        sampleFramebuffer->alloc(width, height, sourceFramebuffer->m_drmFormat);

    outPaddingRatio = Vector2D(static_cast<double>(padding) / width, static_cast<double>(padding) / height);
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbId(sampleFramebuffer));
    const bool useNeighbours = neighbours && neighbours->m_size == sourceFramebuffer->m_size &&
                               neighbours->m_drmFormat == sourceFramebuffer->m_drmFormat;
    const int ownX0 = std::floor(box.x), ownY0 = std::floor(box.y);
    const int ownX1 = std::ceil(box.x + box.width), ownY1 = std::ceil(box.y + box.height);
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
            auto tx = [&](int x) { return cx + (x - ax) * (dx - cx) / (bx - ax); };
            auto ty = [&](int y) { return cy + (y - ay) * (dy - cy) / (by - ay); };
            glBlitFramebuffer(r[0], r[1], r[2], r[3], tx(r[0]), ty(r[1]), tx(r[2]), ty(r[3]), GL_COLOR_BUFFER_BIT, GL_NEAREST);
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

void blurBackground(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer>& blurredFramebuffer,
                    float sigma, SP<Render::IFramebuffer> callerFramebuffer) {
    if (sigma <= 0.0f) {
        blurredFramebuffer.reset();
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(callerFramebuffer));
        return;
    }
    const int sourceWidth = static_cast<int>(sampleFramebuffer->m_size.x);
    const int sourceHeight = static_cast<int>(sampleFramebuffer->m_size.y);
    int width = std::max(1, (sourceWidth + 1) / 2);
    int height = std::max(1, (sourceHeight + 1) / 2);
    auto allocate = [&](SP<Render::IFramebuffer>& framebuffer, int w, int h) {
        if (!framebuffer)
            framebuffer = g_pHyprRenderer->createFB("hyprliquid-blur");
        if (framebuffer->m_size.x != w || framebuffer->m_size.y != h ||
            framebuffer->m_drmFormat != sampleFramebuffer->m_drmFormat)
            framebuffer->alloc(w, h, sampleFramebuffer->m_drmFormat);
    };
    allocate(blurredFramebuffer, width, height);

    // Preserve the half-resolution result, but prefilter into smaller levels
    // until the Gaussian's taps overlap. Merely stretching the fixed kernel
    // across the sharp image leaves a visible grid at larger effect scales.
    std::vector<SP<Render::IFramebuffer>> levels{blurredFramebuffer};
    auto& pyramid = g_pGlobalState->blurPyramid;
    while ((sigma * width / sourceWidth > 2.8f || sigma * height / sourceHeight > 2.8f) &&
           (width > 1 || height > 1)) {
        width = std::max(1, (width + 1) / 2);
        height = std::max(1, (height + 1) / 2);
        const size_t index = levels.size() - 1;
        if (pyramid.size() <= index)
            pyramid.emplace_back();
        allocate(pyramid[index], width, height);
        levels.push_back(pyramid[index]);
    }
    auto& temporary = g_pGlobalState->blurTempFramebuffer;
    allocate(temporary, width, height);

    static constexpr std::array<float, 9> projection{2, 0, 0, 0, 2, 0, -1, -1, 1};
    auto& manager = g_pGlobalState->shaderManager;
    auto shader = g_pHyprOpenGL->useShader(manager.blurShader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, projection);
    shader->setUniformInt(SHADER_TEX, 0);
    glBindVertexArray(shader->getUniformLocation(SHADER_SHADER_VAO));
    g_pHyprOpenGL->setCapStatus(GL_SCISSOR_TEST, false);
    auto draw = [&](const SP<Render::IFramebuffer>& target, const SP<Render::IFramebuffer>& input) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbId(target));
        g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(target->m_size.x), static_cast<int>(target->m_size.y));
        bindTexture(input, GL_TEXTURE0);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };

    glUniform1i(manager.blurUniforms.filterMode, 1);
    SP<Render::IFramebuffer> input = sampleFramebuffer;
    for (const auto& level : levels) {
        draw(level, input);
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

    glBindFramebuffer(GL_FRAMEBUFFER, fbId(callerFramebuffer));
    glBindVertexArray(0);
    g_pHyprOpenGL->setViewport(0, 0, static_cast<int>(callerFramebuffer->m_size.x),
                              static_cast<int>(callerFramebuffer->m_size.y));
}

void applyGlassEffect(SP<Render::IFramebuffer> sampleFramebuffer, SP<Render::IFramebuffer> blurredFramebuffer,
                      SP<Render::IFramebuffer> targetFramebuffer, CBox& rawBox, CBox& transformedBox,
                      float alpha, float cornerRadius, float roundingPower,
                      const Vector2D& paddingRatio, const SResolveContext& resolveContext, const SMaskInfo* mask) {
    if (!sampleFramebuffer || !targetFramebuffer)
        return;
    auto& manager = g_pGlobalState->shaderManager;
    const auto& uniforms = manager.glassUniforms;
    const auto material = resolveMaterial(resolveContext);
    const auto transform = Math::wlTransformToHyprutils(
        Math::invertTransform(g_pHyprRenderer->m_renderData.pMonitor->m_transform));
    Mat3x3 matrix = g_pHyprRenderer->projectBoxToTarget(rawBox, transform);
    matrix.transpose();

    glBindFramebuffer(GL_FRAMEBUFFER, fbId(targetFramebuffer));
    bindTexture(sampleFramebuffer, GL_TEXTURE0);
    bindTexture(blurredFramebuffer ? blurredFramebuffer : sampleFramebuffer, GL_TEXTURE1);
    if (mask && mask->textureId != 0) {
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(mask->target, mask->textureId);
    }
    glActiveTexture(GL_TEXTURE0);

    auto shader = g_pHyprOpenGL->useShader(manager.glassShader);
    shader->setUniformMatrix3fv(SHADER_PROJ, 1, GL_FALSE, matrix.getMatrix());
    shader->setUniformInt(SHADER_TEX, 0);
    shader->setUniformFloat2(SHADER_FULL_SIZE, transformedBox.width, transformedBox.height);
    shader->setUniformFloat(SHADER_RADIUS, cornerRadius);
    shader->setUniformFloat(SHADER_ROUNDING_POWER, roundingPower);
    glUniform1i(uniforms.blurTexture, 1);
    glUniform1f(uniforms.pixelScale, g_pHyprRenderer->m_renderData.pMonitor->m_scale * material.effect_scale);
    glUniform1f(uniforms.fadeAlpha, std::clamp(alpha, 0.0f, 1.0f));
    glUniform2f(uniforms.uvPadding, paddingRatio.x, paddingRatio.y);
#define GLASS_PARAM(name, value, lo, hi) glUniform1f(uniforms.name, material.name);
#include "MaterialParams.inc"
#undef GLASS_PARAM

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
    g_pHyprOpenGL->scissor(rawBox);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    g_pHyprOpenGL->scissor(nullptr);
}

} // namespace GlassRenderer
