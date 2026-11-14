// Pixel-area reference: sample the unfiltered, finite-width linear ramp at
// 256 x 256 subpixels. This does not use the shader's analytic integration.
static double sampledRim(double depth, double dx, double dy, double height, double curvature) {
    if (height <= 0) return 0;
    double sum = 0;
    constexpr int samples = 256;
    for (int y = 0; y < samples; ++y) for (int x = 0; x < samples; ++x) {
        const double d = depth + ((x + .5) / samples - .5) * dx + ((y + .5) / samples - .5) * dy;
        if (d >= 0 && d < height) sum += 1 - curvature * d / height;
    }
    return sum / (samples * samples);
}

static void rimAntialiasing() {
    const std::string prefix = "#version 300 es\nprecision highp float;\n"
        "layout(location = 0) out vec4 fragColor;\n";
    const std::string main = R"(
uniform vec4 testLine;
uniform float testCurvature;
void main() {
    float depth = testLine.z + dot(gl_FragCoord.xy - vec2(1.5), testLine.xy);
    fragColor = vec4(highlightBand(depth, testLine.w, testCurvature));
})";
    const auto source = prefix + read("shaders/liquid-highlight.glsl") + main;
    Program candidate(source, false), previous(frozenHighlightAntialiasing(source), false);
    FloatTarget output;
    int comparisons = 0;
    double error = 0, oldError = 0, peak = 0;
    glDisable(GL_BLEND);
    for (float scale : {.75f, 1.25f, 2.f}) for (float angle : {0.f, 10.f, 30.f, 45.f, 80.f, 90.f, 135.f})
        for (float height : {.01f, .25f, 1.f, 30.f}) for (float curvature : {0.f, .7f, 1.f}) {
            const double radians = angle * std::numbers::pi / 180;
            const float dx = std::cos(radians) / scale, dy = std::sin(radians) / scale;
            const float span = std::abs(dx) + std::abs(dy);
            for (float depth : {-.5f * span, -.17f * span, 0.f, .23f * height, .5f * height,
                                .77f * height, height, height + .17f * span, height + .5f * span}) {
                const double expected = sampledRim(depth, dx, dy, height, curvature);
                auto render = [&](const Program& program) {
                    program.use();
                    glUniform4f(program.loc("testLine"), dx, dy, depth, height);
                    program.f("testCurvature", curvature);
                    return output.draw()[0];
                };
                const double delta = std::abs(render(candidate) - expected);
                peak = std::max(peak, delta); error += delta;
                oldError += std::abs(render(previous) - expected);
                require(delta < .004, "Rim pixel-area error at scale=" + std::to_string(scale)
                    + " angle=" + std::to_string(angle) + " height=" + std::to_string(height)
                    + " curvature=" + std::to_string(curvature) + " depth=" + std::to_string(depth)
                    + " error=" + std::to_string(delta));
                ++comparisons;
            }
        }
    require(error < oldError * .05, "Rim AA did not improve subpixel accuracy");
    candidate.use(); candidate.f("testCurvature", .7);
    for (float height : {0.f, -1.f}) {
        glUniform4f(candidate.loc("testLine"), 1, 0, 0, height);
        require(output.draw()[0] == 0, "Nonpositive rim height must be disabled");
    }
    require(glGetError() == GL_NO_ERROR, "Rim antialiasing GL error");
    std::cout << "PASS: " << comparisons << " rim AA comparisons against 256x256 subpixels, mean error "
              << error / comparisons << " (previous " << oldError / comparisons << "), max " << peak << '\n';
}
