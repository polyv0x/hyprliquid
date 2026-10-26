#pragma once
#include "LiquidMaterial.hpp"
#include <GLES3/gl3.h>
#include <cmath>
#include <numbers>

struct SLiquidUniforms {
    GLint backdrop = -1, cleanBackdrop = -1, outputExtent = -1, renderShadow = -1, transparentTarget = -1;
    GLint refraction = -1, outer = -1, distances = -1, opacities = -1;
    GLint face = -1, faceFill = -1, shadow = -1, shadowOffset = -1, shadowYCC = -1, shadowFill = -1;
    GLint highlight0 = -1, highlight1 = -1, highlight2 = -1, keyColor = -1, fillColor = -1, bias = -1;
    std::array<GLint, 4> rows{};
    void locate(GLuint program) {
        const auto loc = [program](const char* name) { return glGetUniformLocation(program, name); };
        backdrop = loc("backdrop_texture"); cleanBackdrop = loc("clean_backdrop_texture");
        outputExtent = loc("outputExtent"); renderShadow = loc("renderShadow");
        transparentTarget = loc("transparentTarget");
        refraction = loc("uRefraction"); outer = loc("uOuter");
        distances = loc("uBlurDistances[0]"); opacities = loc("uBlurOpacities[0]");
        face = loc("uFace"); faceFill = loc("uFaceFill");
        shadow = loc("uShadow"); shadowOffset = loc("uShadowOffset");
        shadowYCC = loc("uShadowYCC"); shadowFill = loc("uShadowFill");
        highlight0 = loc("uHighlight0"); highlight1 = loc("uHighlight1"); highlight2 = loc("uHighlight2");
        keyColor = loc("uKeyColor"); fillColor = loc("uFillColor"); bias = loc("uHighlightBias");
        rows = {loc("uHighlightRow0"), loc("uHighlightRow1"), loc("uHighlightRow2"), loc("uHighlightRow3")};
    }
    void upload(const SLiquidMaterial& material, float angleDegrees = 0.0f,
                const std::array<float, 4>& framebufferBasis = {1, 0, 0, 1}) const {
        const auto vec4 = [](GLint loc, const auto& value) { glUniform4f(loc, value[0], value[1], value[2], value[3]); };
        const auto direction = [&](double x, double y) {
            return std::array<double, 2>{framebufferBasis[0] * x + framebufferBasis[2] * y,
                                         framebufferBasis[1] * x + framebufferBasis[3] * y};
        };
        const auto& r = material.refraction;
        glUniform4f(refraction, r.innerHeight, r.innerAmount, r.outerHeight, r.outerAmount);
        glUniform3f(outer, r.outerDistances[0], r.outerDistances[1], r.outerOpacity);
        std::array<GLfloat, 5> d, a;
        for (size_t i = 0; i < d.size(); ++i) {
            d[i] = material.blur.distances[i]; a[i] = material.blur.opacities[i] * material.blur.opacity;
        }
        glUniform1fv(distances, 5, d.data()); glUniform1fv(opacities, 5, a.data());
        const auto& f = material.face;
        glUniform4f(face, f.ycc.black, f.ycc.white, f.ycc.saturation, f.opacity);
        vec4(faceFill, f.ycc.normalFill);
        const auto& s = material.shadow;
        glUniform4f(shadow, s.amount, s.height, s.shadowRadius, s.opacity);
        const auto offset = direction(s.offset[0], -s.offset[1]); // material y-down -> framebuffer y-up
        glUniform2f(shadowOffset, offset[0], offset[1]);
        glUniform4f(shadowYCC, s.ycc.black, s.ycc.white, s.ycc.saturation, 1.0f);
        vec4(shadowFill, s.ycc.normalFill);
        const auto& h = material.highlight;
        const double angle = h.appliesDisplayAngle ? angleDegrees * std::numbers::pi / 180.0 : 0.0;
        // Both highlight response coefficients are fixed at zero.
        const auto key = direction(std::sin(h.keyAngle + angle), -std::cos(h.keyAngle + angle));
        const auto fill = direction(std::sin(h.fillAngle + angle), -std::cos(h.fillAngle + angle));
        glUniform4f(highlight0, h.keyHeight, std::cos(h.keySpread), 0.0, key[0]);
        glUniform4f(highlight1, key[1], h.fillHeight, std::cos(h.fillSpread), 0.0);
        glUniform4f(highlight2, fill[0], fill[1], h.curvature, h.inset);
        vec4(keyColor, h.keyColor); vec4(fillColor, h.fillColor);
        std::array<GLfloat, 4> offsets;
        for (size_t i = 0; i < rows.size(); ++i) { vec4(rows[i], h.colorMatrix[i]); offsets[i] = h.colorMatrix[i][4]; }
        glUniform4fv(bias, 1, offsets.data());
    }
};
