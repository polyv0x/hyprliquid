// Compare against the complete pre-optimization shader, including its frozen
// shape/clean-backdrop helpers. Native readback catches changes hidden by PNGs.
static void exactGlass(const Program& candidate) {
    Program original(read("tests/reference/glass-unoptimized.frag"), false);
    Program interior(read("shaders/glass-interior.frag"), false);
    int comparisons = 0;
    for (const auto format : {GL_RGBA8, GL_RGB10_A2, GL_RGBA16F}) {
        for (const float scale : {1.0f, 1.25f, 2.0f}) {
            const int ww = 510 * scale, wh = 310 * scale, pad = 93 * scale;
            const int w = ww + 2 * pad, h = wh + 2 * pad;
            std::vector<unsigned char> noise(w * h * 4), frost(noise.size());
            uint32_t seed = 0x71ab29;
            for (size_t i = 0; i < noise.size(); ++i) {
                seed = 1664525 * seed + 1013904223;
                noise[i] = i % 4 == 3 ? 255 : seed >> 24;
                frost[i] = i % 4 == 3 ? 255 : (seed >> 16) & 255;
            }
            Target sharp(w, h, noise.data(), format), blurred(w, h, frost.data(), format);
            Target before(w, h, nullptr, format), after(w, h, nullptr, format);
            for (const auto* name : {"frosted", "soft-glass", "clear-glass", "frosted-glass", "frosted-glass-clear-edge"})
                for (bool dark : {false, true}) for (int variant = 0; variant < 6; ++variant) {
                    auto material = presetMaterial(name, dark);
                    const float power = std::array{1.0f, 1.5f, 2.0f, 3.6f, 4.4f, 10.0f}[variant];
                    const float fade = std::array{1.0f, .73f, .125f, 0.0f, 1.0f, 1.0f}[variant];
                    if (variant == 4) {
                        material.coverage_width = 4;
                        material.blur_dist3 = 110;
                        material.shadow_offset = -30;
                        material.holding_tone_opacity = .15;
                        material.glass_opacity = .63;
                    }
                    if (variant == 5) {
                        material.refraction_opacity = .67;
                        material.blur_alpha3 = .9;
                        material.neighbor_falloff = 0;
                    }
                    auto render = [&](const Program& program, const Target& target) {
                        copy(sharp, target); target.bind(); glViewport(pad, pad, ww, wh);
                        parameters(program, material, scale, w, h, ww, wh, false, power);
                        program.f("radius", (variant == 2 ? 0 : 56) * scale);
                        program.f("fadeAlpha", fade);
                        sharp.sample(0); blurred.sample(1);
                        glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); quad(); glDisable(GL_BLEND);
                    };
                    render(original, before); render(candidate, after);
                    if (format == GL_RGBA8) if (const auto inset = glassInteriorInset(material, (variant == 2 ? 0 : 56) * scale, scale)) {
                        const int margin = std::ceil(*inset);
                        if (ww > margin * 2 && wh > margin * 2) {
                            glEnable(GL_SCISSOR_TEST);
                            glScissor(pad + margin, pad + margin, ww - 2 * margin, wh - 2 * margin);
                            copy(sharp, after); after.bind(); glViewport(pad, pad, ww, wh);
                            parameters(interior, material, scale, w, h, ww, wh, false, power);
                            interior.f("fadeAlpha", fade);
                            interior.f("interiorCoverage", 1);
                            sharp.sample(0); blurred.sample(1);
                            glEnable(GL_BLEND); quad(); glDisable(GL_BLEND); glDisable(GL_SCISSOR_TEST);
                        }
                    }
                    const auto a = before.nativePixels(), b = after.nativePixels();
                    if (a != b) {
                        size_t count = 0;
                        for (size_t i = 0; i < a.size(); ++i) count += a[i] != b[i];
                        throw std::runtime_error("Exact glass mismatch: " + std::string(name) + " format=" +
                            std::to_string(format) + " scale=" + std::to_string(scale) + " variant=" +
                            std::to_string(variant) + " dark=" + std::to_string(dark) + " bytes=" + std::to_string(count));
                    }
                    ++comparisons;
                }
        }
    }
    std::cout << "PASS: " << comparisons << " byte-identical glass comparisons in RGBA8, RGB10_A2 and RGBA16F.\n";
}
