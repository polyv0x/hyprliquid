// Compare against the Liquid shader, with a flat body so only the rim
// contributes contrast. This covers real compositing, masks and pixel normals.
static void glassRim(const Program& glass) {
    Program liquid(read("shaders/liquid.frag"), false);
    struct Case {
        float angle, width, strength, effect, fade;
        std::array<float, 4> basis;
        bool masked;
    };
    const std::array<Case, 8> cases{{
        {0, 1, 1, 1, 1, {1, 0, 0, 1}, false},
        {90, 1, 1, 1, 1, {1, 0, 0, 1}, false},
        {-45, 1, 1, 1, 1, {1, 0, 0, 1}, false},
        {30, 3, .4f, .5f, .65f, {0, 1, -1, 0}, false},
        {-90, .01f, 1.6f, 2, 1, {-1, 0, 0, 1}, false},
        {180, 30, 1, 1, .5f, {0, -1, 1, 0}, false},
        {90, 1, 0, 1, 1, {1, 0, 0, 1}, false},
        {45, 2, 1, 1, .73f, {1, 0, 0, -1}, true},
    }};
    int comparisons = 0;
    for (float scale : {1.0f, 1.25f, 2.0f}) {
        const int w = 171 * scale, h = 123 * scale;
        std::vector<unsigned char> pixels(w * h * 4), surface(pixels.size(), 0);
        for (int i = 0; i < w * h; ++i) {
            pixels[i * 4] = 45; pixels[i * 4 + 1] = 70; pixels[i * 4 + 2] = 95; pixels[i * 4 + 3] = 255;
            surface[i * 4 + 3] = i % 3 ? 64 : 0;
        }
        Target sharp(w, h, pixels.data()), mask(w, h, surface.data()), actual(w, h), expected(w, h);
        for (bool dark : {false, true}) for (float radius : {0.0f, 24.0f})
            for (float power : {1.0f, 2.0f, 3.6f, 6.0f, 10.0f}) {
                std::vector<unsigned char> zeroAngle;
                for (size_t variant = 0; variant < cases.size(); ++variant) {
                    const auto& c = cases[variant];
                    auto material = presetMaterial("frosted-glass", dark);
                    auto reference = presetMaterial("frosted-liquid", dark);
                    // Leave the default rim values intact in the first two cases.
                    if (variant >= 2) {
                        material.rim_width = reference.rim_width = c.width;
                        material.rim_opacity = reference.rim_opacity = c.strength;
                    }
                    material.face_opacity = material.holding_tone_opacity = 0;
                    material.edge_bleed_opacity = material.shadow_amount = 0;
                    material.inner_refraction_amount = material.outer_refraction_amount = material.aberration_amount = 0;
                    reference.face_opacity = reference.refraction_opacity = 0;
                    const auto data = effectiveLiquidMaterial(reference);
                    auto render = [&](const Program& shader, const Target& output, bool isLiquid) {
                        copy(sharp, output); output.bind();
                        if (isLiquid) {
                            liquidParameters(shader, data, scale * c.effect, w, h, w, h, 0, power);
                            shader.i("renderShadow", 0);
                        } else parameters(shader, material, scale * c.effect, w, h, w, h, false, power);
                        SLiquidHighlightUniforms rim; rim.locate(shader.id);
                        rim.upload(isLiquid ? data.highlight : effectiveLiquidMaterial(material).highlight, c.angle, c.basis);
                        shader.f("radius", radius * scale); shader.f("fadeAlpha", c.fade);
                        shader.i("useMask", c.masked); shader.i("maskMode", 0);
                        shader.v2("maskUVScale", 1, 1); shader.v2("maskUVOffset", 0, 0);
                        shader.f("maskAlphaThreshold", .01f);
                        for (int unit : {0, 1, 6, 7}) sharp.sample(unit);
                        mask.sample(2);
                        glEnable(GL_BLEND); glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); quad(); glDisable(GL_BLEND);
                    };
                    render(glass, actual, false); render(liquid, expected, true);
                    const auto a = actual.pixels(), b = expected.pixels();
                    int maximum = 0;
                    for (size_t i = 0; i < a.size(); ++i)
                        maximum = std::max(maximum, std::abs(int(a[i]) - int(b[i])));
                    require(maximum <= 1, "Glass/Liquid rim mismatch at scale=" + std::to_string(scale)
                            + " dark=" + std::to_string(dark) + " radius=" + std::to_string(radius)
                            + " power=" + std::to_string(power) + " variant=" + std::to_string(variant));
                    if (variant == 0) zeroAngle = a;
                    if (variant == 1) require(a != zeroAngle, "Glass rim angle did not rotate the highlights");
                    if (c.strength == 0) require(a == pixels, "Disabled Glass rim changed the flat backdrop");
                    require(glGetError() == GL_NO_ERROR, "Glass rim OpenGL error");
                    ++comparisons;
                }
            }
    }
    std::cout << "PASS: " << comparisons << " Glass/Liquid rim comparisons across themes, angles, transforms, controls, masks and fades.\n";
}
