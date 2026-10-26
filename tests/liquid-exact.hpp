// Freeze the complete pre-optimization shader and its shape/material helpers.
// Compare native attachment values, including the 10-bit desktop and HDR path.
static void exactLiquid() {
    Program original(read("tests/reference/liquid-unoptimized.frag"), false);
    Program candidate(read("shaders/liquid.frag"), false);
    auto layerSource = read("tests/reference/liquid-unoptimized.frag");
    layerSource.insert(layerSource.find('\n') + 1, "#define LAYER_SHAPES\n");
    Program originalLayer(layerSource, false), candidateLayer(read("shaders/liquid-layer.frag"), false);
    int comparisons = 0;
    for (const auto format : {GL_RGBA8, GL_RGB10_A2, GL_RGBA16F}) {
        for (const float scale : {0.75f, 1.25f, 2.0f}) {
            const int ww = 321 * scale, wh = 219 * scale, pad = 101 * scale;
            const int w = ww + 2 * pad, h = wh + 2 * pad;
            std::vector<unsigned char> noise(w * h * 4), frost(noise.size()), mask(noise.size());
            uint32_t seed = 0x7142ab;
            for (size_t i = 0; i < noise.size(); ++i) {
                seed = 1664525 * seed + 1013904223;
                noise[i] = i % 4 == 3 ? 255 : seed >> 24;
                frost[i] = i % 4 == 3 ? 255 : (seed >> 16) & 255;
                mask[i] = i % 4 == 3 ? (i / 4 % 3) * 64 : 16;
            }
            Target sharp(w, h, noise.data(), format), half((w + 1) / 2, (h + 1) / 2, nullptr, format);
            Target blurred((w + 1) / 2, (h + 1) / 2, nullptr, format), blurInput(w, h, frost.data(), format);
            copy(sharp, half); copy(blurInput, blurred);
            Target before(w, h, nullptr, format), after(w, h, nullptr, format);
            Target surface(w, h, mask.data());
            Target shapes(2, 1);
            const std::array<float, 8> bounds{ww * .15f, wh * .15f, ww * .7f, wh * .7f, 24 * scale, 3.6f, 1, 1};
            shapes.sample(5);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 2, 1, 0, GL_RGBA, GL_FLOAT, bounds.data());
            for (bool layered : {false, true}) for (bool dark : {false, true}) for (int variant = 0; variant < 8; ++variant) {
                auto material = liquidMaterial(dark);
                if (variant == 5) {
                    material.refraction.innerHeight = 80;
                    material.refraction.outerHeight = 50;
                    material.refraction.outerAmount = 70;
                    material.highlight.inset = 12;
                    material.highlight.keyHeight = 16;
                    material.highlight.fillHeight = 13;
                    material.shadow.offset = {17, -27};
                }
                const float power = std::array{1.f, 1.5f, 2.f, 3.6f, 4.4f, 10.f, 3.6f, 2.f}[variant];
                auto render = [&](const Program& program, const Target& target) {
                    copy(sharp, target); target.bind();
                    liquidParameters(program, material, scale, w, h, ww, wh, pad, power);
                    program.f("radius", (variant == 2 ? 0 : variant == 6 ? 500 : 24) * scale);
                    program.f("fadeAlpha", variant == 3 ? .73f : variant == 4 ? 0.f : 1.f);
                    program.f("glass_opacity", variant == 7 ? .4f : 1.f);
                    program.f("neighbor_falloff", variant == 7 ? .7f : 0.f);
                    program.i("useNeighborContribution", variant == 7);
                    program.f("neighbor_contribution", .3f);
                    program.i("underlayCount", variant >= 6 ? 1 : 0);
                    if (layered) {
                        program.i("useMask", 1); program.i("maskMode", 2); program.i("renderShadow", 0);
                        program.f("maskAlphaThreshold", .02f);
                        program.v2("maskUVOffset", 0, 0); program.v2("maskUVScale", 1, 1);
                        program.i("layerShapeCount", 2);
                        const std::array<float, 8> boxes{0, 0, ww * .45f, float(wh), ww * .55f, 0, ww * .45f, float(wh)};
                        const std::array<float, 2> radii{24 * scale, 12 * scale};
                        glUniform4fv(program.loc("layerShapeBounds[0]"), 2, boxes.data());
                        glUniform1fv(program.loc("layerShapeRadii[0]"), 2, radii.data());
                    }
                    sharp.sample(0); half.sample(6); blurred.sample(1);
                    blurInput.sample(7); half.sample(4); shapes.sample(5); surface.sample(2);
                    glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); quad(); glDisable(GL_BLEND);
                };
                render(layered ? originalLayer : original, before); render(layered ? candidateLayer : candidate, after);
                const auto a = before.nativePixels(), b = after.nativePixels();
                if (a != b) {
                    size_t count = 0;
                    for (size_t i = 0; i < a.size(); ++i) count += a[i] != b[i];
                    throw std::runtime_error("Exact Liquid mismatch: format=" + std::to_string(format) +
                        " scale=" + std::to_string(scale) + " dark=" + std::to_string(dark) +
                        " variant=" + std::to_string(variant) + " layered=" + std::to_string(layered) +
                        " bytes=" + std::to_string(count));
                }
                ++comparisons;
            }
        }
    }
    std::cout << "PASS: " << comparisons << " byte-identical Liquid comparisons in RGBA8, RGB10_A2 and RGBA16F.\n";
}
