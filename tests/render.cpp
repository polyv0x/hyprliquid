#include "BuiltInPresets.hpp"
#include "PixelTransfer.hpp"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

static void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}
static std::string read(const char* path) {
    std::ifstream file(path);
    require(file.good(), std::string("Cannot read ") + path);
    std::string source{std::istreambuf_iterator<char>(file), {}};
    for (auto at = source.find("#include \""); at != std::string::npos; at = source.find("#include \"")) {
        const auto end = source.find('"', at + 10);
        require(end != std::string::npos, "Unterminated shader include");
        const auto header = std::filesystem::path(path).parent_path() / source.substr(at + 10, end - at - 10);
        source.replace(at, end - at + 1, read(header.c_str()));
    }
    return source;
}
static GLuint compile(GLenum type, const std::string& source) {
    GLuint shader = glCreateShader(type);
    const char* text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = 0; glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[8192]{}; glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        throw std::runtime_error(log);
    }
    return shader;
}
struct Program {
    GLuint id;
    Program(const std::string& fragment, bool reference) {
        const auto vertex = reference
            ? "attribute vec2 aPos; varying vec2 vUV; void main(){ vUV=aPos; gl_Position=vec4(aPos*2.0-1.0,0.0,1.0); }"
            : "#version 300 es\nin vec2 aPos; out vec2 v_texcoord; void main(){ v_texcoord=aPos; gl_Position=vec4(aPos*2.0-1.0,0.0,1.0); }";
        GLuint vs = compile(GL_VERTEX_SHADER, vertex), fs = compile(GL_FRAGMENT_SHADER, fragment);
        id = glCreateProgram(); glAttachShader(id, vs); glAttachShader(id, fs);
        glBindAttribLocation(id, 0, "aPos"); glLinkProgram(id);
        GLint ok = 0; glGetProgramiv(id, GL_LINK_STATUS, &ok);
        if (!ok) { char log[8192]{}; glGetProgramInfoLog(id, sizeof(log), nullptr, log); throw std::runtime_error(log); }
        glDeleteShader(vs); glDeleteShader(fs);
    }
    void use() const { glUseProgram(id); }
    GLint loc(const char* name) const { return glGetUniformLocation(id, name); }
    void f(const char* n, float x) const { glUniform1f(loc(n), x); }
    void i(const char* n, int x) const { glUniform1i(loc(n), x); }
    void v2(const char* n, float x, float y) const { glUniform2f(loc(n), x, y); }
};
struct Target {
    GLuint texture, framebuffer;
    int width, height;
    Target(int w, int h, const void* data = nullptr): width(w), height(h) {
        glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Incomplete FBO");
    }
    ~Target() { glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture); }
    void bind() const { glBindFramebuffer(GL_FRAMEBUFFER, framebuffer); glViewport(0, 0, width, height); }
    void sample(int unit) const { glActiveTexture(GL_TEXTURE0 + unit); glBindTexture(GL_TEXTURE_2D, texture); }
    std::vector<unsigned char> pixels() const {
        bind(); std::vector<unsigned char> p(width * height * 4);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, p.data()); return p;
    }
};
static void quad() { glDrawArrays(GL_TRIANGLE_STRIP, 0, 4); }
static void copy(const Target& from, const Target& to) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, from.framebuffer);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, to.framebuffer);
    glBlitFramebuffer(0, 0, from.width, from.height, 0, 0, to.width, to.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
}
static void blur(const Program& program, const Target& sharp, Target& temporary, Target& result, float sigma, bool reference) {
    program.use(); program.i(reference ? "uTex" : "tex", 0); glDisable(GL_BLEND);
    const Target* input = &sharp;
    for (int iteration = 0; iteration < 2; ++iteration) {
        temporary.bind(); input->sample(0);
        program.v2(reference ? "uStep" : "direction", sigma / 5.6f / result.width, 0);
        quad();
        result.bind(); temporary.sample(0);
        program.v2(reference ? "uStep" : "direction", 0, sigma / 5.6f / result.height);
        quad(); input = &result;
    }
}
static SMaterial presetMaterial(const std::string& name, bool dark = true) {
    const auto all = BuiltInPresets::getAll();
    SMaterial m;
    std::vector<const SCustomPreset*> chain;
    std::string current = name;
    for (int i = 0; i < 8 && !current.empty(); ++i) {
        auto found = all.find(current); require(found != all.end(), "Missing preset " + current);
        chain.push_back(&found->second); current = found->second.inherits;
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
#define GLASS_PARAM(name, value, lo, hi) if ((*it)->shared.name != SENTINEL_FLOAT) m.name = (*it)->shared.name;
#include "MaterialParams.inc"
#undef GLASS_PARAM
        const auto& variant = dark ? (*it)->dark : (*it)->light;
#define GLASS_PARAM(name, value, lo, hi) if (variant.name != SENTINEL_FLOAT) m.name = variant.name;
#include "MaterialParams.inc"
#undef GLASS_PARAM
    }
    sanitizeMaterial(m); return m;
}
static void parameters(const Program& p, const SMaterial& m, float scale, int width, int height, int ww, int wh,
                       bool reference, float roundingPower) {
    p.use();
#define GLASS_PARAM(name, value, lo, hi) p.f(#name, m.name);
#include "MaterialParams.inc"
#undef GLASS_PARAM
    if (!reference) {
        p.i("tex", 0); p.i("blur_texture", 1); p.i("maskTex", 2); p.i("useMask", 0);
        p.i("clean_texture", 3); p.i("clean_blur_texture", 4); p.i("useNeighborContribution", 0);
        p.i("hasCleanBlur", 1);
        glUniform2i(p.loc("cleanOrigin"), 0, 0);
        glUniform4i(p.loc("cleanOwnerBounds"), -1000, -1000, -500, -500);
        p.i("underlayShapes", 5); p.i("underlayCount", 0);
        p.v2("fullSize", ww, wh);
        p.v2("uvPadding", float(width - ww) / (2 * width), float(height - wh) / (2 * height));
        p.f("radius", 56 * scale); p.f("roundingPower", roundingPower); p.f("pixelScale", scale); p.f("fadeAlpha", 1);
        return;
    }
    // The frozen study fixture retains its comparison switch. Production
    // always uses the affine face tone, so pin the reference to that endpoint.
    p.f("use_measured_face", 1.0f);
    p.i("source_texture", 0); p.i("blur_texture", 1);
    p.v2("uRes", width, height); p.v2("uInvRes", 1.0f / width, 1.0f / height);
    p.v2("uCenter", width * 0.5f, height * 0.5f); p.v2("uHalf", ww * 0.5f, wh * 0.5f);
    p.f("uCornerRadius", 56 * scale); p.f("uSuperness", roundingPower);
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, float>>{
        {"outer_refraction_amount", m.outer_refraction_amount}, {"inner_refraction_amount", m.inner_refraction_amount},
        {"refraction_threshold0", m.refraction_threshold0}, {"refraction_threshold1", m.refraction_threshold1},
        {"edge_bleed_amount", m.edge_bleed_amount}, {"shadow_offset", m.shadow_offset},
        {"aberration_amount", m.aberration_amount}, {"rim_width", m.rim_width},
        {"coverage_offset", m.coverage_offset}, {"coverage_width", m.coverage_width}}) p.f(name, value * scale);
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, float>>{
        {"outer_refraction_inv_height", m.outer_refraction_inv_height}, {"inner_refraction_inv_height", m.inner_refraction_inv_height},
        {"edge_bleed_inv_height", m.edge_bleed_inv_height}, {"shadow_inv_height", m.shadow_inv_height},
        {"inv_aberration_height", m.inv_aberration_height}}) p.f(name, value / scale);
    const float a = m.blur_alpha0, b = m.blur_alpha3, d = m.blur_dist3 * scale;
    glUniform4f(p.loc("blur_alpha"), a, a * 0.92f + b * 0.08f, a * 0.45f + b * 0.55f, b);
    glUniform4f(p.loc("blur_dist"), 0, d * 0.22f, d * 0.55f, d);
    p.v2("aberration_angle", 0.9239f, 0.3827f); p.v2("uLightDir", -0.5736f, 0.8192f);
    glUniform3f(p.loc("rim_color"), 1, 0.99f, 0.96f);
}
static void save(const std::string& name, const Target& target) {
    auto pixels = target.pixels(); std::ofstream file(name, std::ios::binary);
    file << "P6\n" << target.width << " " << target.height << "\n255\n";
    for (int y = target.height - 1; y >= 0; --y)
        for (int x = 0; x < target.width; ++x) file.write(reinterpret_cast<char*>(pixels.data() + (y * target.width + x) * 4), 3);
}

static std::string outlineOptics(std::string source) {
    // Keep the original optical path as a comparison, without changing the
    // frozen reference fixtures or adding a test switch to the shipped shader.
    const std::string call = "vec2 opticalN = optical_normal(p, depth, n);";
    const auto at = source.find(call);
    require(at != std::string::npos, "Missing optical normal call");
    source.replace(at, call.size(), "vec2 opticalN = n;");
    return source;
}

static void opticalCorners(const Program& native, const Program& legacy) {
    int comparisons = 0;
    for (const float scale : {1.0f, 1.25f, 2.0f})
        for (const float radius : {0.0f, 12.0f})
            for (const float power : {1.0f, 2.0f, 4.0f, 10.0f}) {
                const int ww = 256 * scale, wh = 192 * scale, pad = 160 * scale;
                const int w = ww + 2 * pad, h = wh + 2 * pad;
                std::vector<unsigned char> gradient(w * h * 4);
                for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                    const int i = (y * w + x) * 4;
                    gradient[i] = 20 + 180 * x / (w - 1);
                    gradient[i + 1] = 20 + 180 * y / (h - 1);
                    gradient[i + 2] = 40; gradient[i + 3] = 255;
                }
                Target sharp(w, h, gradient.data()), before(w, h), after(w, h);
                auto m = presetMaterial("lens");
                // A coordinate gradient makes refraction discontinuities
                // measurable without depending on the desktop wallpaper.
                m.blur_alpha0 = m.blur_alpha3 = m.face_opacity = m.aberration_amount = 0;
                m.edge_bleed_opacity = m.shadow_amount = m.rim_opacity = 0;
                auto render = [&](const Program& shader, const Target& backdrop, const Target& output) {
                    copy(backdrop, output); output.bind(); glViewport(pad, pad, ww, wh);
                    parameters(shader, m, scale, w, h, ww, wh, false, power);
                    shader.f("radius", radius * scale);
                    backdrop.sample(0); backdrop.sample(1); glDisable(GL_BLEND); quad();
                };
                render(legacy, sharp, before); render(native, sharp, after);
                const auto oldPixels = before.pixels(), newPixels = after.pixels();
                for (const bool right : {false, true}) for (const bool top : {false, true}) {
                    auto creaseStep = [&](const auto& pixels) {
                        int maximum = 0;
                        const int localY = int(20 * scale);
                        const int y = pad + (top ? wh - 1 - localY : localY);
                        for (int offset = int(15 * scale); offset < int(25 * scale); ++offset) {
                            const int x = pad + (right ? ww - 1 - offset : offset);
                            const int nextX = x + (right ? -1 : 1);
                            for (int c = 0; c < 2; ++c)
                                maximum = std::max(maximum, std::abs(int(pixels[(y * w + x) * 4 + c])
                                    - int(pixels[(y * w + nextX) * 4 + c])));
                        }
                        return maximum;
                    };
                    const int oldStep = creaseStep(oldPixels), newStep = creaseStep(newPixels);
                    require(oldStep >= 5, "Refraction fixture did not expose the old corner seam");
                    require(newStep <= 3 && newStep < oldStep * 0.5f, "Corner refraction still has a sharp step");
                    ++comparisons;
                }
                // Straight edges keep their original refraction. This strip
                // includes the silhouette and its antialiasing transition.
                for (int y = pad - 2; y < pad + int(80 * scale); ++y)
                    for (int x = pad + int(110 * scale); x < pad + int(146 * scale); ++x)
                        for (int c = 0; c < 4; ++c)
                            require(std::abs(int(oldPixels[(y * w + x) * 4 + c])
                                - int(newPixels[(y * w + x) * 4 + c])) <= 1, "Straight edge changed");

                // A uniform backdrop removes directional sampling differences:
                // coverage, depth-based shading and rim lighting must be exact.
                for (int i = 0; i < w * h; ++i) {
                    gradient[i * 4] = 65; gradient[i * 4 + 1] = 85;
                    gradient[i * 4 + 2] = 105;
                }
                Target solid(w, h, gradient.data());
                m = presetMaterial("platform");
                render(legacy, solid, before); render(native, solid, after);
                require(before.pixels() == after.pixels(), "Coverage, rim or material shading changed");

                if (scale == 1 && radius == 12 && power == 4) {
                    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                        const int i = (y * w + x) * 4;
                        gradient[i] = 30 + (x / 21 % 2) * 110;
                        gradient[i + 1] = 20 + (y / 27 % 2) * 100;
                        gradient[i + 2] = 40 + (x + y) % 130;
                    }
                    Target grid(w, h, gradient.data());
                    m = presetMaterial("lens");
                    m.blur_alpha0 = m.blur_alpha3 = m.face_opacity = 0;
                    render(legacy, grid, before); render(native, grid, after);
                    save("build/corners-before.ppm", before); save("build/corners-after.ppm", after);
                }
                require(glGetError() == GL_NO_ERROR, "Optical corner OpenGL error");
            }
    std::cout << "PASS: " << comparisons << " corner continuity checks, unchanged straight edges, coverage and rim.\n";
}

static void blurBrightness(const Program& shader) {
    int comparisons = 0;
    for (const auto& color : std::array<std::array<unsigned char, 4>, 3>{{
             {0, 1, 2, 255}, {30, 60, 240, 255}, {128, 254, 255, 255}}}) {
        std::vector<unsigned char> pixels(193 * 129 * 4);
        for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = color[i % 4];
        Target sharp(193, 129, pixels.data()), half(97, 65), coarse(49, 33);
        Target temporary(49, 33), blurred(49, 33);
        for (float sigma : {0.1f, 8.0f, 48.0f}) {
            shader.use(); shader.i("tex", 0); shader.i("cleanInput", 0); shader.i("clean_texture", 0);
            shader.i("filterMode", 1); glDisable(GL_BLEND);
            half.bind(); sharp.sample(0); quad();
            coarse.bind(); half.sample(0); quad();
            shader.i("filterMode", 0);
            blur(shader, coarse, temporary, blurred, sigma, false);
            shader.i("filterMode", 2);
            half.bind(); blurred.sample(0); quad();
            const auto result = half.pixels();
            for (size_t i = 0; i < result.size(); ++i)
                require(result[i] == color[i % 4], "Blur changed the brightness of a flat color");
            ++comparisons;
        }
    }
    shader.use(); shader.i("filterMode", 0);
    require(glGetError() == GL_NO_ERROR, "Blur brightness OpenGL error");
    std::cout << "PASS: " << comparisons << " flat-color brightness checks through reduction, four Gaussian passes and reconstruction.\n";
}

static void neighborFalloff(const Program& shader) {
    // Two equally bright/dark stripes have the same mean as their blurred
    // image. Follow them inward across the native/neighbor sampling boundary:
    // detail must only decrease, and their mean must never fade to wallpaper.
    constexpr std::array<unsigned char, 4> mean{120, 140, 160, 255}, wallpaper{20, 40, 60, 255};
    SMaterial material;
    material.face_opacity = material.rim_opacity = material.shadow_amount = 0;
    material.edge_bleed_opacity = material.holding_tone_opacity = 0;
    material.aberration_amount = 0;
    material.outer_refraction_amount = material.inner_refraction_amount = 40;
    material.outer_refraction_inv_height = material.inner_refraction_inv_height = 0;
    material.blur_alpha0 = material.blur_alpha3 = 0.2f;
    int comparisons = 0;
    for (float scale : {1.0f, 1.25f, 2.0f}) for (float effect : {1.0f, 1.5f}) {
        const int ww = 256 * scale, h = 320 * scale, padding = 80 * scale, w = ww + 2 * padding;
        std::vector<unsigned char> scenePixels(w * h * 4), blurredPixels(w * h * 4), cleanPixels(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) for (int c = 0; c < 4; ++c) {
            const auto i = (y * w + x) * 4 + c;
            scenePixels[i] = mean[c] + (c < 3 ? (y < h / 2 ? 60 : -60) : 0);
            blurredPixels[i] = mean[c];
            cleanPixels[i] = wallpaper[c];
        }
        Target scene(w, h, scenePixels.data()), blurred(w, h, blurredPixels.data());
        Target clean(w, h, cleanPixels.data()), result(ww, h);
        for (float rate : {0.0f, 0.25f, 0.5f, 1.0f}) for (bool separate : {true, false}) {
            material.neighbor_falloff = rate;
            result.bind(); parameters(shader, material, scale * effect, w, h, ww, h, false, 2);
            shader.f("radius", 0); shader.i("useNeighborContribution", separate ? 1 : 0);
            glUniform4i(shader.loc("cleanOwnerBounds"), padding, 0, padding + ww, h);
            scene.sample(0); blurred.sample(1); clean.sample(3); clean.sample(4);
            glDisable(GL_BLEND); quad();
            const auto pixels = result.pixels();
            int previousContrast = 255;
            for (int x = 3; x < 80 * scale; ++x) {
                const float depth = (x + 0.5f) / (scale * effect);
                const float detail = 0.8f * std::exp2(-depth * rate / 8.0f);
                const auto bright = (int(96 * scale) * ww + x) * 4;
                const auto dark = (int(224 * scale) * ww + x) * 4;
                for (int c = 0; c < 3; ++c) {
                    require(std::abs((pixels[bright + c] + pixels[dark + c]) * 0.5f - mean[c]) <= 0.5f,
                            "Reflection brightness dipped at the neighbor/native handoff");
                    require(std::abs(pixels[bright + c] - (mean[c] + 60 * detail)) <= 0.6f &&
                            std::abs(pixels[dark + c] - (mean[c] - 60 * detail)) <= 0.6f,
                            "Reflection detail did not soften continuously into frost");
                }
                const int contrast = pixels[bright] - pixels[dark];
                require(contrast <= previousContrast, "Reflection detail reappeared inside the pane");
                previousContrast = contrast;
                require(pixels[bright + 3] == 255 && pixels[dark + 3] == 255, "Falloff changed glass opacity");
            }
            ++comparisons;
        }
        // Fully frosted pixels and glass alpha are unaffected. Zero blur also
        // makes falloff a no-op because there is no softer image to blend in.
        auto frost = presetMaterial("clear-edge");
        frost.blur_alpha0 = frost.blur_alpha3 = 1;
        std::vector<unsigned char> baseline;
        for (bool zeroBlur : {false, true}) {
            if (zeroBlur) frost = material;
            for (float rate : {0.0f, 1.0f}) {
                frost.neighbor_falloff = rate;
                result.bind(); parameters(shader, frost, scale * effect, w, h, ww, h, false, 3.6f);
                shader.f("fadeAlpha", 0.6f);
                scene.sample(0); (zeroBlur ? scene : blurred).sample(1);
                glDisable(GL_BLEND); quad();
                if (rate > 0)
                    require(result.pixels() == baseline, "Falloff changed full frost, zero blur or opacity");
                else
                    baseline = result.pixels();
            }
        }
        require(glGetError() == GL_NO_ERROR, "Neighbour falloff OpenGL error");
    }
    std::cout << "PASS: " << comparisons << " brightness-preserving reflection handoffs, monotonic detail, unchanged frost/opacity/zero blur.\n";
}

static void underlayProtection(const Program& shader) {
    constexpr std::array<unsigned char, 4> sceneColor{180, 80, 60, 255}, cleanColor{20, 120, 200, 255};
    int comparisons = 0;
    for (float scale : {1.0f, 1.25f, 2.0f}) {
        const int w = 256 * scale, h = 192 * scale;
        std::vector<unsigned char> scenePixels(w * h * 4), cleanPixels(w * h * 4);
        for (int i = 0; i < w * h; ++i) for (int c = 0; c < 4; ++c) {
            scenePixels[i * 4 + c] = sceneColor[c];
            cleanPixels[i * 4 + c] = cleanColor[c];
        }
        Target scene(w, h, scenePixels.data()), clean(w, h, cleanPixels.data()), result(w, h);
        GLuint shapes;
        glGenTextures(1, &shapes);
        glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, shapes);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        // Put the relevant shape second, to exercise multiple underlying windows.
        for (bool rounded : {false, true}) for (bool rayLeaves : {false, true}) {
            std::array<float, 16> geometry{1000, 1000, 10, 10, 0, 2, 0, 0};
            const std::array<float, 8> lower = rounded
                ? std::array<float, 8>{0, 0, 80, 80, 20, 2, 0, 0}
                : std::array<float, 8>{rayLeaves ? 0.0f : -80.0f, 0, rayLeaves ? 40.0f : 120.0f, 192, 0, 2, 0, 0};
            for (int i = 0; i < 8; ++i)
                geometry[i + 8] = lower[i] * (i < 5 ? scale : 1);
            glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, shapes);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 2, 2, 0, GL_RGBA, GL_FLOAT, geometry.data());
            for (float contribution : {0.0f, 0.5f, 1.0f}) for (float bleed : {0.0f, 1.0f}) {
                SMaterial material;
                material.face_opacity = material.rim_opacity = material.shadow_amount = 0;
                material.holding_tone_opacity = material.aberration_amount = material.bleed_darken = 0;
                material.blur_alpha0 = material.blur_alpha3 = 0;
                material.outer_refraction_amount = material.inner_refraction_amount = rounded ? 0 : 60;
                material.outer_refraction_inv_height = material.inner_refraction_inv_height = 0;
                material.edge_bleed_amount = rounded ? 0 : 60;
                material.edge_bleed_inv_height = 0;
                material.edge_bleed_opacity = bleed;
                material.neighbor_contribution = contribution;
                material.neighbor_falloff = 1;
                result.bind(); parameters(shader, material, scale, w, h, w, h, false, 2);
                shader.f("radius", 0); shader.i("useNeighborContribution", 1); shader.i("underlayCount", 2);
                scene.sample(0); scene.sample(1); clean.sample(3); clean.sample(4);
                glDisable(GL_BLEND); quad();
                const auto pixels = result.pixels();
                for (bool covered : {false, true}) {
                    const int x = (rounded ? (covered ? 20 : 4) : (covered ? 10 : 50)) * scale;
                    const int y = (rounded ? (covered ? 20 : 4) : 96) * scale;
                    const bool protectedRay = covered && (rounded || !rayLeaves);
                    const float weight = protectedRay ? 1 : contribution;
                    for (int c = 0; c < 3; ++c) {
                        const float expected = cleanColor[c] + weight * (sceneColor[c] - cleanColor[c]);
                        require(std::abs(pixels[(y * w + x) * 4 + c] - expected) <= 0.6f,
                                "Underlying window faded, or an exposed neighbour bypassed its controls");
                    }
                    ++comparisons;
                }
            }
        }
        glDeleteTextures(1, &shapes); glActiveTexture(GL_TEXTURE0);
        require(glGetError() == GL_NO_ERROR, "Underlay protection OpenGL error");
    }
    std::cout << "PASS: " << comparisons << " underlying-window, exposed-neighbour, rounded-corner and ray-exit checks.\n";
}

static void cleanSampling() {
    Program sceneShader(read("shaders/scene-sample.frag"), false);
    Program blurShader(read("shaders/blur.frag"), false);
    const std::string prefix = "#version 300 es\nprecision highp float; uniform sampler2D tex;\n";
    const std::string interface = "\nin vec2 v_texcoord; layout(location=0) out vec4 fragColor;\n";
    Program direct(prefix + interface + "void main(){ fragColor=vec4(texture(tex,v_texcoord).rgb,1); }", false);
    Program reconstructed(prefix + read("shaders/clean-backdrop.glsl") + interface +
                          "void main(){ fragColor=vec4(sample_clean(v_texcoord),1); }", false);
    constexpr int w = 19, h = 13, sw = 29, sh = 23;
    std::array<std::vector<unsigned char>, 3> inputs;
    for (int source = 0; source < 3; ++source) {
        inputs[source].resize(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c)
                inputs[source][(y * w + x) * 4 + c] = (x * 17 + y * 29 + source * 71 + c * 43) % 256;
    }
    Target native(w, h, inputs[0].data()), scene(w, h, inputs[1].data()), background(w, h, inputs[2].data());
    Target full(sw, sh), materialized(sw, sh);
    int comparisons = 0;
    for (const auto& owner : std::array<std::array<int, 4>, 3>{{{3, 2, 15, 10}, {-2, -1, 6, 8}, {13, 8, 23, 17}}}) {
        const int ox = owner[0] - 5, oy = owner[1] - 5;
        for (bool clean : {false, true}) {
            (clean ? materialized : full).bind(); sceneShader.use();
            sceneShader.i("tex", 0); sceneShader.i("sceneTex", 1); sceneShader.i("backgroundTex", 2);
            glUniform2i(sceneShader.loc("sampleOrigin"), ox, oy);
            glUniform4iv(sceneShader.loc("ownerBounds"), 1, owner.data());
            sceneShader.f("neighborContribution", clean ? 0 : 1);
            native.sample(0); scene.sample(1); background.sample(2);
            glDisable(GL_BLEND); quad();
        }
        for (const auto& size : std::array<std::array<int, 2>, 3>{{{sw, sh}, {42, 30}, {(sw + 1) / 2, (sh + 1) / 2}}}) {
            Target expected(size[0], size[1]), actual(size[0], size[1]);
            for (bool reduction : {false, true}) {
                for (bool optimized : {false, true}) {
                    (optimized ? actual : expected).bind();
                    const auto& shader = reduction ? blurShader : optimized ? reconstructed : direct;
                    shader.use(); shader.i("tex", 0); shader.i("clean_texture", 1);
                    shader.i("filterMode", 1); shader.i("cleanInput", optimized ? 1 : 0);
                    glUniform2i(shader.loc("cleanOrigin"), ox, oy);
                    glUniform4iv(shader.loc("cleanOwnerBounds"), 1, owner.data());
                    (optimized ? full : materialized).sample(0); background.sample(1);
                    quad();
                }
                const auto a = actual.pixels(), e = expected.pixels();
                for (size_t i = 0; i < a.size(); ++i)
                    require(std::abs(int(a[i]) - int(e[i])) <= 1,
                            "Reconstructed clean sharp/blur differs at owner boundary or offscreen gutter");
                ++comparisons;
            }
        }
    }
    require(glGetError() == GL_NO_ERROR, "Clean sampling OpenGL error");
    glActiveTexture(GL_TEXTURE0);
    std::cout << "PASS: " << comparisons << " clean sharp/fused reduction comparisons, including bilinear boundaries and offscreen gutters.\n";
}

static void pixelTransfers() {
    constexpr int w = 3, h = 2;
    std::array<unsigned char, w * h * 4> pixels{}, result{};
    for (size_t i = 0; i < pixels.size(); ++i) pixels[i] = i * 7;
    Target target(w, h);
    GLuint buffer = 0;
    glGenBuffers(1, &buffer);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, buffer);
    glBufferData(GL_PIXEL_UNPACK_BUFFER, 256, nullptr, GL_STATIC_DRAW);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 8);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 9);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 3);
    glPixelStorei(GL_UNPACK_SKIP_ROWS, 2);
    {
        CPixelTransferGuard<true> unpack;
        glBindTexture(GL_TEXTURE_2D, target.texture);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
    GLint value = 0;
    glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &value); require(value == GLint(buffer), "Unpack buffer was not restored");
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &value); require(value == 9, "Unpack row length was not restored");
    glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &value); require(value == 3, "Unpack offset was not restored");
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4); glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0); glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 8); glPixelStorei(GL_PACK_ROW_LENGTH, 9);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 3); glPixelStorei(GL_PACK_SKIP_ROWS, 2);
    target.bind();
    {
        CPixelTransferGuard<false> pack;
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, result.data());
    }
    glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &value); require(value == GLint(buffer), "Pack buffer was not restored");
    glGetIntegerv(GL_PACK_ROW_LENGTH, &value); require(value == 9, "Pack row length was not restored");
    glGetIntegerv(GL_PACK_SKIP_ROWS, &value); require(value == 2, "Pack offset was not restored");
    require(result == pixels && glGetError() == GL_NO_ERROR, "Pixel transfer used inherited memory offsets");
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0); glDeleteBuffers(1, &buffer);
    glPixelStorei(GL_PACK_ALIGNMENT, 4); glPixelStorei(GL_PACK_ROW_LENGTH, 0);
    glPixelStorei(GL_PACK_SKIP_PIXELS, 0); glPixelStorei(GL_PACK_SKIP_ROWS, 0);
    std::cout << "PASS: bounded pixel transfers with inherited row offsets and pixel buffers\n";
}

static void sceneSampling() {
    Program shader(read("shaders/scene-sample.frag"), false);
    // Distinct native/scene/background images catch use of an earlier-drawn
    // neighbour as the zero-strength backdrop. Asymmetric sizes expose flips.
    constexpr int w = 19, h = 13, sw = 29, sh = 23;
    std::array<std::vector<unsigned char>, 3> inputs;
    for (int source = 0; source < 3; ++source) {
        inputs[source].resize(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x)
            for (int c = 0; c < 4; ++c)
                inputs[source][(y * w + x) * 4 + c] = (x * 17 + y * 29 + source * 71 + c * 43) % 256;
    }
    Target native(w, h, inputs[0].data()), scene(w, h, inputs[1].data()), background(w, h, inputs[2].data());
    Target sample(sw, sh);
    int comparisons = 0;
    for (const auto& owner : std::array<std::array<int, 4>, 3>{{{3, 2, 15, 10}, {-2, -1, 6, 8}, {13, 8, 23, 17}}}) {
        const int ox = owner[0] - 5, oy = owner[1] - 5;
        for (float strength : {0.0f, 0.25f, 0.5f, 1.0f}) {
            sample.bind(); shader.use();
            shader.i("tex", 0); shader.i("sceneTex", 1); shader.i("backgroundTex", 2);
            glUniform2i(shader.loc("sampleOrigin"), ox, oy);
            glUniform4iv(shader.loc("ownerBounds"), 1, owner.data());
            shader.f("neighborContribution", strength);
            native.sample(0); scene.sample(1); background.sample(2);
            glDisable(GL_BLEND); quad();
            const auto result = sample.pixels();
            for (int y = 0; y < sh; ++y) for (int x = 0; x < sw; ++x) {
                const int px = std::clamp(x + ox, 0, w - 1), py = std::clamp(y + oy, 0, h - 1);
                const bool inside = px >= owner[0] && py >= owner[1] && px < owner[2] && py < owner[3];
                for (int c = 0; c < 4; ++c) {
                    const int src = (py * w + px) * 4 + c, dst = (y * sw + x) * 4 + c;
                    const float expected = inside ? inputs[0][src] :
                        inputs[2][src] * (1 - strength) + inputs[1][src] * strength;
                    require(std::abs(result[dst] - expected) <= (inside ? 0 : 0.51f),
                            "Scene sampling changed owner pixels, neighbour strength or offscreen gutters");
                }
            }
            require(glGetError() == GL_NO_ERROR, "Scene sampling OpenGL error");
            ++comparisons;
        }
    }
    std::cout << "PASS: " << comparisons << " neighbour blends, exact owner pixels and clamped offscreen gutters.\n";
}

int main() try {
    auto display = eglGetPlatformDisplay(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    require(display != EGL_NO_DISPLAY && eglInitialize(display, nullptr, nullptr), "Cannot initialize surfaceless EGL");
    require(eglBindAPI(EGL_OPENGL_ES_API), "Cannot bind GLES");
    EGLConfig config; EGLint count;
    const EGLint attributes[]{EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
    require(eglChooseConfig(display, attributes, &config, 1, &count) && count, "No GLES3 EGL config");
    const EGLint contextAttributes[]{EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, contextAttributes);
    require(context != EGL_NO_CONTEXT && eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, context), "Cannot create GLES3 context");
    std::cout << "Renderer: " << glGetString(GL_RENDERER) << '\n';
    Program native(read("shaders/glass.frag"), false), reference(read("tests/reference/glass.frag"), true);
    Program legacy(outlineOptics(read("shaders/glass.frag")), false);
    Program nativeBlur(read("shaders/blur.frag"), false), referenceBlur(read("tests/reference/blur.frag"), true);
    GLuint vao, vbo; glGenVertexArrays(1, &vao); glBindVertexArray(vao); glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    const float vertices[]{0,0, 1,0, 0,1, 1,1}; glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glDisable(GL_DITHER);
    pixelTransfers();
    sceneSampling();
    blurBrightness(nativeBlur);
    neighborFalloff(native);
    underlayProtection(native);
    cleanSampling();
    bool passed = true;
    int comparisons = 0;
    std::vector<std::string> presets;
    for (const auto& [name, preset] : BuiltInPresets::getAll())
        if (!preset.hidden) presets.push_back(name);
    std::ranges::sort(presets);
    // Cover output scaling and Hyprland's full corner-power range, including
    // triangular and intermediate corners that must not be clamped to circles.
    for (const auto [scale, roundingPower] : std::initializer_list<std::pair<float, float>>{
             {1.0f, 3.6f}, {1.5f, 3.6f}, {2.0f, 3.6f},
             {1.5f, 1.0f}, {1.5f, 1.5f}, {1.5f, 2.0f}, {1.5f, 4.4f}, {1.5f, 10.0f}}) {
        const int ww = 400 * scale, wh = 240 * scale, pad = 160 * scale;
        const int w = ww + 2 * pad, h = wh + 2 * pad;
        std::vector<unsigned char> pixels(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const int i = (y * w + x) * 4;
            pixels[i] = 30 + (x / 21 % 2) * 110;
            pixels[i+1] = 20 + (y / 27 % 2) * 100;
            pixels[i+2] = 40 + (x + y) % 130; pixels[i+3] = 255;
        }
        Target sharp(w, h, pixels.data()), temp(w/2, h/2), blurred(w/2, h/2), refBlur(w/2, h/2);
        Target actual(w, h), expected(w, h);
        for (const auto& name : presets) for (const bool dark : {true, false}) {
            auto m = presetMaterial(name, dark);
            // The frozen WebGL reference predates reflection falloff, which
            // is exercised independently above.
            m.neighbor_falloff = 0;
            const std::string theme = dark ? "dark" : "light";
            require(m.face_slope == (dark ? 0.72f : 0.68f)
                    && m.face_intercept == (dark ? 0.06f : 0.30f), "Preset lost its theme tones: " + name);
            require(name != "soft-glass" || m.inner_refraction_amount == -7.0f, "Negative refraction was lost");
            blur(nativeBlur, sharp, temp, blurred, m.blur_sigma * scale, false);
            blur(referenceBlur, sharp, temp, refBlur, m.blur_sigma * scale, true);
            require(blurred.pixels() == refBlur.pixels(), "Blur differs from WebGL");
            expected.bind(); parameters(reference, m, scale, w, h, ww, wh, true, roundingPower);
            sharp.sample(0); refBlur.sample(1); glDisable(GL_BLEND); quad();
            copy(sharp, actual); actual.bind(); glViewport(pad, pad, ww, wh);
            parameters(legacy, m, scale, w, h, ww, wh, false, roundingPower); sharp.sample(0); blurred.sample(1);
            glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); quad(); glDisable(GL_BLEND);
            const auto a = actual.pixels(), b = expected.pixels();
            int maximum = 0; double sum = 0; int samples = 0;
            for (int y = pad + 2; y < pad + wh - 2; ++y) for (int x = pad + 2; x < pad + ww - 2; ++x)
                for (int channel = 0; channel < 3; ++channel) {
                    const int i = (y * w + x) * 4 + channel;
                    const int difference = std::abs(int(a[i]) - int(b[i])); maximum = std::max(maximum, difference);
                    sum += difference; ++samples;
                }
            const double mean = sum / samples;
            std::cout << name << " (" << theme << ") @ " << scale << "x, rounding " << roundingPower
                      << ": mean " << mean << "/255, max " << maximum << "/255\n";
            passed &= mean < 0.15 && maximum <= 3;
            ++comparisons;
            if (scale == 1.0f) {
                copy(sharp, actual); actual.bind(); glViewport(pad, pad, ww, wh);
                parameters(native, m, scale, w, h, ww, wh, false, roundingPower); sharp.sample(0); blurred.sample(1);
                glEnable(GL_BLEND); quad(); glDisable(GL_BLEND);
                save("build/" + name + "-" + theme + "-native.ppm", actual); save("build/" + name + "-" + theme + "-webgl.ppm", expected);
            }
            // A completely faded decoration must leave the backdrop byte-for-byte intact.
            copy(sharp, actual); actual.bind(); glViewport(pad, pad, ww, wh);
            parameters(native, m, scale, w, h, ww, wh, false, roundingPower); native.f("fadeAlpha", 0);
            sharp.sample(0); blurred.sample(1); glEnable(GL_BLEND); quad(); glDisable(GL_BLEND);
            require(actual.pixels() == pixels, "Fade zero changed the backdrop");
            require(glGetError() == GL_NO_ERROR, "OpenGL error");
        }
    }
    require(passed, "Shader parity exceeded tolerance");
    std::cout << "PASS: " << comparisons << " reference comparisons with outline optics, exact blur parity, transparent fade.\n";
    opticalCorners(native, legacy);
    return 0;
} catch (const std::exception& e) {
    std::cerr << "FAIL: " << e.what() << '\n'; return 1;
}
