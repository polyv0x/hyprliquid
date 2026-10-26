// Independent equations and reference comparisons for the Liquid family.
// Included by render.cpp so both families exercise the same GLES context.
struct FloatTarget {
    GLuint texture, framebuffer;
    FloatTarget() {
        glGenTextures(1, &texture); glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 4, 4, 0, GL_RGBA, GL_FLOAT, nullptr);
        glGenFramebuffers(1, &framebuffer); glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        require(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE, "Floating-point analytic target unavailable");
    }
    ~FloatTarget() { glDeleteFramebuffers(1, &framebuffer); glDeleteTextures(1, &texture); }
    std::array<float, 4> draw() const {
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer); glViewport(0, 0, 4, 4); quad();
        std::array<float, 4> result;
        glReadPixels(1, 1, 1, 1, GL_RGBA, GL_FLOAT, result.data());
        for (float value : result) require(std::isfinite(value), "Non-finite material output");
        return result;
    }
};

static void liquidAnalytic() {
    auto source = read("shaders/liquid.frag");
    source.erase(source.find("void main()"));
    Program equations(source + R"(
uniform float testDistance;
void main() {
    float inner = refractAmount(-testDistance, uRefraction.x, uRefraction.y);
    float outer = refractAmount(-testDistance, uRefraction.z, uRefraction.w);
    fragColor = vec4(inner, outer, blurWeight(testDistance),
        liquidSaturate((testDistance - uOuter.x) / (uOuter.y - uOuter.x)) * uOuter.z);
})", false);
    equations.use(); SLiquidUniforms u; u.locate(equations.id); u.upload(LIQUID_DEFAULT);
    FloatTarget output;
    for (const auto& [distance, expected] : std::initializer_list<std::pair<float, float>>{
        {0, -60}, {-10, -8.03847577f}, {-20, 0}, {-100, 0}}) {
        equations.f("testDistance", distance);
        const auto pixel = output.draw();
        require(std::abs(pixel[0] - expected) < 0.0001 && pixel[1] == 0, "Square-root refraction or disabled outer height");
    }
    for (const auto& [distance, expected] : std::initializer_list<std::pair<float, float>>{
        {-84, 1}, {-83, 1}, {-82.999f, 0.999993902f}, {-42, 0.75f},
        {-1.001f, 0.500006098f}, {-1, 0.5f}, {-0.999f, 0.5f}, {-0.001f, 0.5f}, {0, 1}, {0.001f, 1}}) {
        equations.f("testDistance", distance);
        const auto pixel = output.draw();
        require(std::abs(pixel[2] - expected) < 0.00001, "Five-stop/repeated-stop boundary");
    }
    equations.f("testDistance", -0.5f);
    require(std::abs(output.draw()[3] - 0.1f) < 1e-6, "Signed outer threshold blend");
    glUniform4f(u.refraction, 0, -60, -1, 75);
    require(output.draw()[0] == 0 && output.draw()[1] == 0, "Nonpositive heights must disable rays");

    Program rays(source + R"(
void main() {
    float d = -0.5;
    float inner = refractAmount(-d, uRefraction.x, uRefraction.y);
    fragColor = vec4(blurWeight(d + inner), blurWeight(d), 0.0, 1.0);
})", false);
    rays.use(); u.locate(rays.id); u.upload(LIQUID_DEFAULT);
    auto pixel = output.draw();
    require(pixel[0] > 0.77 && pixel[0] < 0.79 && pixel[1] == 0.5, "Rays must evaluate blur at separate displaced distances");

    Program colors(source + R"(
uniform vec4 testColor;
void main() { fragColor = vec4(approximateYCC(testColor.rgb, uFace.xyz, uFaceFill), 1.0); }
)", false);
    for (bool dark : {false, true}) {
        colors.use(); u.locate(colors.id); u.upload(liquidMaterial(dark));
        glUniform4f(colors.loc("testColor"), 0.2, 0.4, 0.6, 1);
        pixel = output.draw();
        const double y = 0.2126 * 0.2 + 0.7152 * 0.4 + 0.0722 * 0.6;
        const auto& f = liquidMaterial(dark).face;
        for (int c = 0; c < 3; ++c) {
            double expected = (f.ycc.black + (f.ycc.white - f.ycc.black) *
                (y + f.ycc.saturation * ((0.2 + c * 0.2) - y))) * 0.9 + (dark ? 0 : 0.1);
            require(std::abs(pixel[c] - expected) < 1e-6, "YCC/fill approximation");
        }
    }
    Program highlight(source + R"(
uniform vec4 testShape;
void main() { fragColor = approximateHighlightMatrix(keyFillHighlight(testShape, uHighlight0, uHighlight1, uHighlight2, uKeyColor, uFillColor)); }
)", false);
    for (bool dark : {false, true}) {
        highlight.use(); u.locate(highlight.id); u.upload(liquidMaterial(dark));
        glUniform4f(highlight.loc("testShape"), -0.5, 0, 0, 1);
        pixel = output.draw();
        require(pixel == std::array<float, 4>{}, "Vanishing gradient/zero-alpha bias must not add color");
        glUniform4f(highlight.loc("testShape"), -0.5, 0, -1, 1);
        auto lit = output.draw();
        require(std::abs(lit[3] - 0.65f) < 1e-5 && lit[0] > 0.5f, "Directional key band/curvature");
        u.upload(liquidMaterial(dark), 90);
        require(output.draw()[3] < 1e-5, "Display angle did not rotate lighting");
        glUniform4f(highlight.loc("testShape"), -0.5, 1, 0, 1);
        require(std::abs(output.draw()[3] - lit[3]) < 1e-5, "Rotated key strength");
        auto keyOnly = liquidMaterial(dark);
        keyOnly.highlight.fillColor = {};
        u.upload(keyOnly, -90);
        require(output.draw()[3] < 1e-5, "Negative display angle did not reverse lighting");
        glUniform4f(highlight.loc("testShape"), -0.5, -1, 0, 1);
        require(std::abs(output.draw()[3] - lit[3]) < 1e-5, "Negative-angle key strength");
    }
    require(glGetError() == GL_NO_ERROR, "Liquid analytic GL error");
    Program underlays(source + R"(
uniform vec2 receiver;
void main() { fragColor = vec4(underlayWeights(receiver, vec2(0.5), vec2(1.2, 0.5)), 0.0, 1.0); }
)", false);
    Target shapes(2, 1); shapes.sample(5);
    const std::array<float, 8> bounds{0, 0, 100, 100, 20, 2, 1, 0};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 2, 1, 0, GL_RGBA, GL_FLOAT, bounds.data());
    underlays.use(); underlays.i("underlayShapes", 5); underlays.i("underlayCount", 1);
    underlays.i("useNeighborContribution", 1); underlays.v2("fullSize", 100, 100);
    underlays.v2("receiver", 0.5, 0.5);
    pixel = output.draw();
    require(pixel[0] == 1 && pixel[1] == 0, "Each refraction ray needs independent underlay classification");
    underlays.v2("receiver", 1.2, 0.5);
    pixel = output.draw();
    require(pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0, "Exposed receiver must not inherit underlay protection");
    std::cout << "PASS: refraction depths, disabled heights, signed thresholds, separate rays, repeated stops, YCC, key/fill and zero-alpha bias\n";
}

static void liquidParameters(const Program& shader, const SLiquidMaterial& data, float scale,
                             int w, int h, int ww, int wh, float extent, float power = 2) {
    auto material = presetMaterial("frosted-liquid-medium");
    parameters(shader, material, scale, w, h, ww, wh, false, power);
    shader.i("backdrop_texture", 6); shader.i("clean_backdrop_texture", 7);
    shader.v2("outputExtent", extent, extent); shader.i("renderShadow", 1);
    shader.f("radius", 24 * scale);
    SLiquidUniforms u; u.locate(shader.id); u.upload(data);
}

static void liquidBlurResponse(const Program& shader) {
    Program reduction(read("shaders/material-backdrop.frag"), false);
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        const int w = int(640 * scale) + 1, h = 65;
        const int hw = (w + 1) / 2, hh = (h + 1) / 2;
        const float sigma = 6 * scale;
        std::vector<unsigned char> pixels(w * h * 4, 0);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            for (int c = 0; c < 3; ++c) pixels[(y*w+x)*4+c] = x >= w/2 ? 255 : 0;
            pixels[(y*w+x)*4+3] = 255;
        }
        Target sharp(w, h, pixels.data()), half(hw, hh), expected(hw, hh), referenceTemp(hw, hh);
        half.bind(); reduction.use(); reduction.i("tex", 0); reduction.i("useScene", 0); sharp.sample(0); quad();
        shader.use(); shader.i("filterMode", 0); shader.i("cleanInput", 0);
        blur(shader, half, referenceTemp, expected, sigma, false);
        // Characterize the production reduction/Gaussian/reconstruction plan
        // against direct half-resolution filtering, using a non-quantized step.
        Target actual(hw, hh);
        std::vector<std::unique_ptr<Target>> reduced;
        std::vector<Target*> levels{&actual};
        int lw = hw, lh = hh;
        while ((sigma * lw / w > 2.8f || sigma * lh / h > 2.8f) && (lw > 1 || lh > 1)) {
            lw = std::max(1, (lw+1)/2); lh = std::max(1, (lh+1)/2);
            reduced.push_back(std::make_unique<Target>(lw, lh)); levels.push_back(reduced.back().get());
        }
        Target temporary(lw, lh);
        shader.use();
        const Target* input = &half;
        for (auto* target : levels) {
            shader.i("filterMode", target == &actual ? 3 : 1);
            target->bind(); input->sample(0); quad(); input = target;
        }
        const float sx = float(w) / lw, sy = float(h) / lh;
        const float bx = float(w) / hw, by = float(h) / hh;
        const float adjustedX = std::sqrt(std::max(0.0f, sigma*sigma - 0.4f * (sx*sx - bx*bx)));
        const float adjustedY = std::sqrt(std::max(0.0f, sigma*sigma - 0.4f * (sy*sy - by*by)));
        shader.i("filterMode", 0);
        for (int i = 0; i < 2; ++i) {
            temporary.bind(); input->sample(0); shader.v2("direction", adjustedX / (2.8f*w), 0); quad();
            input->bind(); temporary.sample(0); shader.v2("direction", 0, adjustedY / (2.8f*h)); quad();
        }
        shader.i("filterMode", 2);
        for (size_t i = levels.size()-1; i > 0; --i) { levels[i-1]->bind(); levels[i]->sample(0); quad(); }
        shader.i("filterMode", 0);
        auto width = [&](const Target& target) {
            const auto image = target.pixels();
            auto crossing = [&](double threshold) {
                for (int x = 1; x < hw; ++x) {
                    const int a = image[((hh/2)*hw+x-1)*4], b = image[((hh/2)*hw+x)*4];
                    if (a < threshold && b >= threshold) return (x - 1.0 + (threshold-a)/(b-a)) * w / hw / scale;
                }
                throw std::runtime_error("Missing blur step crossing");
            };
            return crossing(229.5) - crossing(25.5);
        };
        const double native = width(actual), reference = width(expected);
        std::cout << "Liquid blur 10-90% width @ " << scale << ": native " << native << ", reference " << reference << " logical pixels\n";
        require(native > 13 && native < 19 && std::abs(native-reference) < 2, "Liquid blur kernel broadened unexpectedly");
    }
}

static void liquidMasks(const Program& shader) {
    constexpr int w = 180, h = 120;
    std::vector<unsigned char> scene(w*h*4, 80), mask(w*h*4, 0);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const int i = (y*w+x)*4;
        scene[i+3] = 255;
        if (x > 50 && x < 100) { mask[i] = 120; mask[i+3] = 128; }
        if (x > 120) { mask[i+1] = 200; mask[i+3] = 255; }
    }
    Target sharp(w, h, scene.data()), surface(w, h, mask.data()), actual(w, h);
    for (int mode : {0, 1}) {
        auto data = LIQUID_DEFAULT; data.highlight.keyColor = {}; data.highlight.fillColor = {};
        copy(sharp, actual); actual.bind();
        liquidParameters(shader, data, 1, w, h, w, h, 0);
        shader.f("radius", 0); shader.i("renderShadow", 0);
        shader.i("useMask", 1); shader.i("maskMode", mode); shader.f("maskAlphaThreshold", 0.01);
        shader.v2("maskUVOffset", 0, 0); shader.v2("maskUVScale", 1, 1);
        shader.i("regionRectCount", 1); glUniform4f(shader.loc("regionRects[0]"), 20, 0, 80, h);
        for (int unit : {0, 1, 4, 6, 7}) sharp.sample(unit);
        surface.sample(2); glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); quad(); glDisable(GL_BLEND);
        const auto output = actual.pixels();
        for (int y = 5; y < h-5; ++y) for (int x = 5; x < w-5; ++x) {
            const int i = (y*w+x)*4;
            if (x < 18) for (int c = 0; c < 4; ++c)
                require(output[i+c] == scene[i+c], "Layer mask leaked glass outside selected area");
            if (x > 125) for (int c = 0; c < 4; ++c)
                require(output[i+c] == mask[i+c], "Layer opaque content was filtered");
            if (x > 25 && x < 45)
                require(mode == 1 ? output[i] > scene[i] : output[i] == scene[i], "Protocol/alpha mask selection");
        }
    }
    std::cout << "PASS: Liquid alpha/region masks and unfiltered opaque layer content\n";
}

static void liquidLayerShapes(const Program& windowShader) {
    Program layerShader(read("shaders/liquid-layer.frag"), false);
    int comparisons = 0;
    for (float scale : {1.0f, 1.25f, 1.6f, 2.0f}) {
        const int w = 400 * scale, h = 600 * scale;
        std::array<std::array<float, 4>, 3> boxes{{{20, 20, 180, 100}, {20, 160, 180, 80}, {240, 20, 120, 140}}};
        std::array<float, 3> radii{20, 12, 24};
        for (auto& b : boxes) for (float& v : b) v *= scale;
        for (float& r : radii) r *= scale;
        const auto distance = [&](int x, int y) {
            double d = 1e30;
            for (size_t i = 0; i < boxes.size(); ++i) {
                const auto& b = boxes[i];
                const double qx = std::abs(x + .5 - b[0] - b[2] / 2) - b[2] / 2 + radii[i];
                const double qy = std::abs(y + .5 - b[1] - b[3] / 2) - b[3] / 2 + radii[i];
                d = std::min(d, std::min(std::max(qx, qy), 0.0) + std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) - radii[i]);
            }
            return d;
        };
        std::vector<unsigned char> scene(w * h * 4);
        for (size_t i = 0; i < scene.size(); i += 4) {
            scene[i] = 64; scene[i+1] = 96; scene[i+2] = 128; scene[i+3] = 255;
        }
        Target sharp(w, h, scene.data()), actual(w, h), expected(w, h);
        for (bool dark : {false, true}) for (float fade : {1.0f, 0.35f, 0.0f}) {
            // A native rounded-window draw is the independent reference for
            // each card's material. The layer has large transparent margins.
            copy(sharp, expected); expected.bind();
            for (size_t i = 0; i < boxes.size(); ++i) {
                const auto& b = boxes[i];
                liquidParameters(windowShader, liquidMaterial(dark), scale, b[2], b[3], b[2], b[3], 0);
                windowShader.f("radius", radii[i]); windowShader.i("renderShadow", 0); windowShader.f("fadeAlpha", fade);
                for (int unit : {0, 1, 4, 6, 7}) sharp.sample(unit);
                glViewport(b[0], b[1], b[2], b[3]);
                glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); quad(); glDisable(GL_BLEND);
            }
            auto reference = expected.pixels();
            std::vector<unsigned char> surface(w * h * 4, 0);
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const int at = (y * w + x) * 4;
                if (distance(x, y) < -1) {
                    const bool text = x > 60 * scale && x < 160 * scale && y > 50 * scale && y < 60 * scale;
                    const int a = text ? 255 : 38;
                    surface[at] = std::round((text ? 230 : 4) * fade);
                    surface[at+1] = std::round((text ? 180 : 5) * fade);
                    surface[at+2] = std::round((text ? 120 : 6) * fade);
                    surface[at+3] = std::round(a * fade);
                }
                for (int c = 0; c < 3; ++c)
                    reference[at+c] = std::round(surface[at+c] + reference[at+c] * (1.0 - surface[at+3] / 255.0));
            }
            Target mask(w, h, surface.data());
            copy(sharp, actual); actual.bind();
            liquidParameters(layerShader, liquidMaterial(dark), scale, w, h, w, h, 0);
            layerShader.i("renderShadow", 0); layerShader.f("fadeAlpha", fade);
            layerShader.i("useMask", 1); layerShader.i("maskMode", 2); layerShader.f("maskAlphaThreshold", 0);
            layerShader.v2("maskUVOffset", 0, 0); layerShader.v2("maskUVScale", 1, 1);
            layerShader.i("layerShapeCount", boxes.size());
            glUniform4fv(layerShader.loc("layerShapeBounds[0]"), boxes.size(), boxes[0].data());
            glUniform1fv(layerShader.loc("layerShapeRadii[0]"), radii.size(), radii.data());
            for (int unit : {0, 1, 4, 6, 7}) sharp.sample(unit);
            mask.sample(2);
            glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); quad(); glDisable(GL_BLEND);
            const auto output = actual.pixels();
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const double d = distance(x, y);
                const int at = (y * w + x) * 4;
                if (d > 1) for (int c = 0; c < 4; ++c)
                    require(output[at+c] == scene[at+c], "Rounded layer leaked into a corner, gap or transparent padding");
                if (d < 0) for (int c = 0; c < 4; ++c)
                    require(std::abs(int(output[at+c]) - int(reference[at+c])) <= 2, "Layer card material differs from its rounded-window reference");
                if (surface[at+3] == 255) for (int c = 0; c < 4; ++c)
                    require(output[at+c] == surface[at+c], "Layer material changed opaque foreground content");
            }
            require(glGetError() == GL_NO_ERROR, "Rounded layer shader GL error");
            ++comparisons;
        }
    }
    std::cout << "PASS: " << comparisons << " rounded layer/native-window comparisons, stacked cards, gaps, scaling, fade and opaque content\n";
}

static void liquidShadow(const Program& shader) {
    // Changing the material's sampled scene must never copy that content into
    // the exterior shadow. Its only effect there is local destination shading.
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const int ww = 180 * scale, wh = 100 * scale, pad = 112 * scale;
        const int w = ww + 2 * pad, h = wh + 2 * pad;
        std::vector<unsigned char> pixels(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const int i = (y * w + x) * 4;
            const unsigned char value = (x / 13 + y / 19) % 2 ? 0 : 192;
            pixels[i] = pixels[i + 1] = pixels[i + 2] = value; pixels[i + 3] = 255;
        }
        const std::array<unsigned char, 4> red{255, 0, 0, 255}, blue{0, 0, 255, 255};
        Target sharp(w, h, pixels.data()), first(1, 1, red.data()), second(1, 1, blue.data());
        Target actual(w, h);
        for (bool dark : {false, true}) {
            const auto& data = liquidMaterial(dark);
            auto render = [&](const Target& material) {
                copy(sharp, actual); actual.bind();
                liquidParameters(shader, data, scale, w, h, ww, wh, pad);
                sharp.sample(0);
                for (int unit : {1, 4, 6, 7}) material.sample(unit);
                glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); quad(); glDisable(GL_BLEND);
                return actual.pixels();
            };
            const auto a = render(first), b = render(second);
            auto distance = [&](double x, double y) {
                const double qx = std::abs(x) - (ww / (2.0 * scale) - 24);
                const double qy = std::abs(y) - (wh / (2.0 * scale) - 24);
                return std::min(std::max(qx, qy), 0.0) + std::hypot(std::max(qx, 0.0), std::max(qy, 0.0)) - 24;
            };
            int shaded = 0, interiorChanged = 0;
            for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
                const double px = (x + 0.5 - w / 2.0) / scale, py = (y + 0.5 - h / 2.0) / scale;
                const double d = distance(px, py);
                const int i = (y * w + x) * 4;
                if (d < -2) { interiorChanged += a[i] != b[i]; continue; }
                if (d <= 2) continue; // Keep coverage/highlight antialiasing out of the shadow assertion.
                const double sd = distance(px + data.shadow.offset[0], py - data.shadow.offset[1]);
                const double coverage = sd < 4 * data.shadow.shadowRadius
                    ? 0.5 * std::exp(-0.5 * std::pow(std::max(sd, 0.0) / data.shadow.shadowRadius, 2)) * data.shadow.opacity : 0;
                for (int c = 0; c < 3; ++c) {
                    require(a[i + c] == b[i + c], "Exterior shadow copied displaced material content");
                    const int expected = std::lround(pixels[i + c] * (1 - coverage));
                    require(std::abs(int(a[i + c]) - expected) <= 1, "Exterior shadow must only darken the local destination");
                }
                require(a[i + 3] == 255 && b[i + 3] == 255, "Shadow changed destination alpha");
                shaded += int(pixels[i]) - int(a[i]) >= 2;
            }
            require(shaded > 100 && interiorChanged > 100, "Shadow isolation fixture must exercise both shadow and refraction");
        }
    }
    std::cout << "PASS: exterior shadows preserve local content, Gaussian opacity and offset in both themes at four scales\n";
}

static void liquidControls(const Program& shader, const Program& blurShader) {
    Program baseline(read("tests/reference/liquid-medium-baseline.frag"), false);
    Program reduction(read("shaders/material-backdrop.frag"), false);
    int defaultComparisons = 0, controlComparisons = 0;
    for (float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        const int ww = 300 * scale, wh = 180 * scale, pad = 210 * scale;
        const int w = ww + 2 * pad, h = wh + 2 * pad;
        std::vector<unsigned char> pixels(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const int i = (y * w + x) * 4;
            pixels[i] = x / 5 % 2 ? 170 : 30; pixels[i + 1] = y / 7 % 2 ? 145 : 25;
            pixels[i + 2] = (x + y) / 9 % 2 ? 175 : 15; pixels[i + 3] = 255;
        }
        Target sharp(w, h, pixels.data()), half((w+1)/2, (h+1)/2), temp((w+1)/2, (h+1)/2), blurred((w+1)/2, (h+1)/2), actual(w, h);
        half.bind(); reduction.use(); reduction.i("tex", 0); reduction.i("useScene", 0); sharp.sample(0); quad();
        auto render = [&](const Program& program, const SMaterial& material, const SLiquidMaterial& data, float power, float fade) {
            if (data.blur.radius > 0) blur(blurShader, half, temp, blurred, data.blur.radius * scale, false);
            copy(sharp, actual); actual.bind();
            liquidParameters(program, data, scale, w, h, ww, wh, pad, power);
            program.f("fadeAlpha", fade); program.f("neighbor_falloff", material.neighbor_falloff);
            sharp.sample(0); half.sample(6); half.sample(7);
            (data.blur.radius > 0 ? blurred : half).sample(1);
            (data.blur.radius > 0 ? blurred : half).sample(4);
            glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); quad(); glDisable(GL_BLEND);
            return actual.pixels();
        };
        for (bool dark : {false, true}) {
            const auto material = presetMaterial("frosted-liquid-medium", dark);
            for (float power : {2.0f, 4.4f}) for (float fade : {0.5f, 1.0f}) {
                const auto expected = render(baseline, material, liquidMaterial(dark), power, fade);
                require(render(shader, material, effectiveLiquidMaterial(material), power, fade) == expected,
                        "Liquid preset changed from the frozen native baseline");
                ++defaultComparisons;
            }
            if (scale != 1.5f) continue;
            struct Edit { const char* name; float SMaterial::* field; float value; };
            for (const auto& edit : std::initializer_list<Edit>{
                {"blur_sigma", &SMaterial::blur_sigma, 14}, {"blur_alpha0", &SMaterial::blur_alpha0, 0},
                {"blur_alpha3", &SMaterial::blur_alpha3, 0}, {"blur_dist3", &SMaterial::blur_dist3, 25},
                {"inner_refraction_amount", &SMaterial::inner_refraction_amount, -20},
                {"inner_refraction_inv_height", &SMaterial::inner_refraction_inv_height, 0.2f},
                {"outer_refraction_amount", &SMaterial::outer_refraction_amount, 45},
                {"outer_refraction_inv_height", &SMaterial::outer_refraction_inv_height, 0.2f},
                {"refraction_threshold0", &SMaterial::refraction_threshold0, 8},
                {"refraction_threshold1", &SMaterial::refraction_threshold1, 40},
                {"refraction_opacity", &SMaterial::refraction_opacity, 0}, {"face_opacity", &SMaterial::face_opacity, 0},
                {"rim_width", &SMaterial::rim_width, 5}, {"rim_opacity", &SMaterial::rim_opacity, 0},
                {"shadow_amount", &SMaterial::shadow_amount, 0.6f}, {"shadow_offset", &SMaterial::shadow_offset, -20}}) {
                auto before = material;
                if (edit.field == &SMaterial::outer_refraction_inv_height) before.outer_refraction_amount = 45;
                const auto expected = render(shader, before, effectiveLiquidMaterial(before), 3.6f, 1);
                auto edited = before; edited.*edit.field = edit.value; sanitizeMaterial(edited);
                const auto changed = render(shader, edited, effectiveLiquidMaterial(edited), 3.6f, 1);
                size_t differences = 0;
                for (size_t i = 0; i < changed.size(); ++i) differences += changed[i] != expected[i];
                require(differences > 8, std::string("Liquid control has no visible effect: ") + edit.name);
                ++controlComparisons;
            }
            auto unblurred = material; unblurred.blur_sigma = 0;
            const auto expected = render(shader, unblurred, effectiveLiquidMaterial(unblurred), 3.6f, 1);
            unblurred.blur_alpha0 = 0; unblurred.blur_alpha3 = 0; unblurred.neighbor_falloff = 1;
            require(render(shader, unblurred, effectiveLiquidMaterial(unblurred), 3.6f, 1) == expected,
                    "Zero blur must use the same retained material input for every blur weight");
        }
    }
    std::cout << "PASS: " << defaultComparisons << " pixel-identical Liquid defaults and " << controlComparisons << " visible control edits, including zero blur\n";
}

static void liquidRendering(const Program& blurShader) {
    liquidAnalytic();
    liquidBlurResponse(blurShader);
    Program native(read("shaders/liquid.frag"), false);
    liquidControls(native, blurShader);
    liquidMasks(native);
    liquidLayerShapes(native);
    liquidShadow(native);
    Program reference(read("tests/reference/liquid-medium.frag"), true);
    Program reduction(read("shaders/material-backdrop.frag"), false);
    // Isolate the material equations from the documented native corner stabilization.
    auto directSource = read("shaders/liquid.frag");
    const std::string optical = "optical_normal(p, max(-d, 0.0), normal)";
    require(directSource.find(optical) != std::string::npos, "Missing native optical-normal expression");
    directSource.replace(directSource.find(optical), optical.size(), "normal");
    Program direct(directSource, false);
    int comparisons = 0;
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        const int ww = 300 * scale, wh = 120 * scale, pad = 160 * scale;
        const int w = ww + 2 * pad, h = wh + 2 * pad;
        std::vector<unsigned char> pixels(w * h * 4);
        for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
            const int i = (y * w + x) * 4;
            pixels[i] = 30 + x * 140 / w; pixels[i+1] = 25 + y * 140 / h;
            pixels[i+2] = 35 + (x / 20 % 2) * 60; pixels[i+3] = 255;
        }
        Target sharp(w, h, pixels.data()), half((w+1)/2, (h+1)/2), temp((w+1)/2, (h+1)/2), blurred((w+1)/2, (h+1)/2);
        Target expected(w, h), actual(w, h);
        half.bind(); reduction.use(); reduction.i("tex", 0); reduction.i("useScene", 0);
        sharp.sample(0); quad();
        const auto retained = half.pixels();
        blur(blurShader, half, temp, blurred, 6 * scale, false);
        require(half.pixels() == retained, "Blur overwrote retained material input");
        for (bool dark : {false, true}) {
            const auto& data = liquidMaterial(dark);
            // The frozen reference displaces exterior shadow content. Compare
            // its body/highlight equations independently of our local shadow,
            // whose spatial behavior and opacity are checked by liquidShadow.
            auto withoutShadow = data;
            withoutShadow.shadow.opacity = 0;
            expected.bind(); reference.use();
            SLiquidUniforms u; u.locate(reference.id); u.upload(withoutShadow);
            reference.i("source_texture", 0); reference.i("backdrop_texture", 6); reference.i("blur_texture", 1);
            reference.v2("uRes", w, h); reference.v2("uCenter", w/2.0f, h/2.0f);
            reference.v2("uHalf", ww/2.0f, wh/2.0f); reference.f("uScale", scale); reference.f("uCornerRadius", 24);
            sharp.sample(0); half.sample(6); blurred.sample(1); glDisable(GL_BLEND); quad();
            copy(sharp, actual); actual.bind();
            liquidParameters(direct, withoutShadow, scale, w, h, ww, wh, pad);
            sharp.sample(0); half.sample(6); half.sample(7); blurred.sample(1); blurred.sample(4);
            glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); quad(); glDisable(GL_BLEND);
            const auto a = actual.pixels(), b = expected.pixels();
            double sum = 0; int maximum = 0;
            for (size_t i = 0; i < a.size(); ++i) {
                const int delta = std::abs(int(a[i]) - int(b[i])); sum += delta; maximum = std::max(maximum, delta);
            }
            std::cout << "Liquid " << (dark ? "dark" : "default") << " @ " << scale << ": mean " << sum / a.size() << ", max " << maximum << '\n';
            require(sum / a.size() < 0.08 && maximum <= 8, "Liquid independent reference mismatch");
            ++comparisons;
            for (float power : {1.0f, 2.0f, 3.6f, 4.4f, 10.0f}) {
                copy(sharp, actual); actual.bind();
                liquidParameters(native, data, scale, w, h, ww, wh, pad, power);
                glEnable(GL_BLEND); quad(); glDisable(GL_BLEND);
                auto output = actual.pixels();
                require(output != pixels, "Liquid material did not render");
                // Every corner has finite alpha and untouched pixels beyond shadow support.
                require(output[3] == 255 && output[0] == pixels[0], "Output bounds/alpha changed the far backdrop");
                native.f("fadeAlpha", 0); copy(sharp, actual); actual.bind();
                glEnable(GL_BLEND); quad(); glDisable(GL_BLEND);
                require(actual.pixels() == pixels, "Liquid zero fade changed destination");
            }
            if (scale == 1) {
                actual.bind(); liquidParameters(native, data, scale, w, h, ww, wh, pad, 3.6f);
                glEnable(GL_BLEND); quad(); glDisable(GL_BLEND);
                save(std::string("build/liquid-medium-") + (dark ? "dark" : "light") + ".ppm", actual);
            }
        }
    }
    // Tiny opposing edges, fractional scale, odd texture sizes and uniform-color conservation.
    for (const auto [ww, wh] : {std::pair{7, 9}, std::pair{171, 123}}) {
        const int pad = 200, w = ww + 2 * pad, h = wh + 2 * pad;
        std::vector<unsigned char> pixels(w * h * 4, 128);
        for (int i = 3; i < int(pixels.size()); i += 4) pixels[i] = 255;
        Target sharp(w, h, pixels.data()), actual(w, h);
        for (float falloff : {0.0f, 1.0f}) for (float contribution : {0.0f, 0.5f, 1.0f}) {
            auto data = LIQUID_DEFAULT; data.face.opacity = 0; data.shadow.opacity = 0;
            data.highlight.keyColor = {}; data.highlight.fillColor = {};
            copy(sharp, actual); actual.bind(); liquidParameters(native, data, 1.25, w, h, ww, wh, pad, 10);
            native.f("neighbor_falloff", falloff); native.f("neighbor_contribution", contribution);
            native.i("useNeighborContribution", 1);
            for (int unit : {0, 1, 4, 6, 7}) sharp.sample(unit);
            glEnable(GL_BLEND); quad(); glDisable(GL_BLEND);
            require(actual.pixels() == pixels, "Liquid rays/falloff changed flat color");
        }
    }
    require(glGetError() == GL_NO_ERROR, "Liquid rendering GL error");
    std::cout << "PASS: " << comparisons << " Liquid reference comparisons, scales/corners, retained backdrop, fades and flat colors\n";
}
