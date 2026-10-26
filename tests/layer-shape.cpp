#include "LayerShape.hpp"
#include <cassert>
#include <iostream>
#include <limits>

// Protocol fixture: quantized horizontal strips, as published by Noctalia.
static std::vector<LayerShape::Rect> card(int x, int y, int w, int h, double radius, int step = 1) {
    std::vector<LayerShape::Rect> result;
    const int band = std::ceil(radius);
    auto append = [&](int top, int bottom, double depth) {
        const double dy = std::max(0.0, radius - depth);
        const int inset = std::ceil(radius - std::sqrt(std::max(0.0, radius * radius - dy * dy)) - 1e-9);
        LayerShape::Rect row{double(x + inset), double(y + top), double(x + w - inset), double(y + bottom)};
        // Canonical regions coalesce adjacent equal spans.
        if (!result.empty() && result.back().x1 == row.x1 && result.back().x2 == row.x2 && result.back().y2 == row.y1)
            result.back().y2 = row.y2;
        else result.push_back(row);
    };
    for (int row = 0; row < band; row += step) append(row, std::min(row + step, band), std::min(row + step, band));
    if (h > 2 * band) append(band, h - band, radius);
    for (int row = h - band; row < h; row += step) append(row, std::min(row + step, h), h - row);
    return result;
}

int main() {
    int checks = 0;
    for (double radius : {0.0, 4.0, 12.0, 18.0, 20.0, 24.0, 11.5, 19.2}) {
        for (int step : {1, 2, 4}) {
            auto strips = card(12, 16, 360, 100, radius, step);
            auto shapes = LayerShape::recognize(strips);
            assert(shapes.size() == 1);
            const auto& s = shapes[0];
            assert(s.x == 12 && s.y == 16 && s.w == 360 && s.h == 100);
            if (strips.size() == 1) assert(s.radius == 0); // No corner information survives coarse strips.
            else {
                if (std::abs(s.radius - radius) > 1.0) std::cerr << "radius " << radius << " step " << step << " recovered " << s.radius << '\n';
                assert(std::abs(s.radius - radius) <= 1.0);
            }
            if (step <= 2 && (radius == 20 || radius == 12)) assert(s.radius == radius);
            auto second = card(12, 132, 360, 84, 12, step);
            strips.insert(strips.end(), second.begin(), second.end());
            auto third = card(400, 20, 180, 160, 20, step);
            strips.insert(strips.end(), third.begin(), third.end());
            shapes = LayerShape::recognize(strips);
            assert(shapes.size() == 3);
            assert(shapes[1].y == 132 && shapes[2].x == 400);
            // Input order must not change recognition (pixman normally sorts).
            std::reverse(strips.begin(), strips.end());
            assert(LayerShape::recognize(strips).size() == 3);
            ++checks;
        }
    }
    assert(LayerShape::recognize({}).empty());
    auto hole = card(0, 0, 360, 100, 20);
    hole.erase(hole.begin() + hole.size() / 2);
    assert(LayerShape::recognize(hole).empty());
    auto asymmetric = card(0, 0, 360, 100, 20);
    asymmetric.front().x1 += 3;
    assert(LayerShape::recognize(asymmetric).empty());
    auto notched = card(0, 0, 360, 100, 20);
    for (auto& r : notched) if (r.y1 < 50 && r.y2 > 50) { r.x1 += 10; r.x2 -= 10; }
    assert(LayerShape::recognize(notched).empty());
    std::vector<LayerShape::Rect> bad{{0, 0, std::numeric_limits<double>::infinity(), 4}};
    assert(LayerShape::recognize(bad).empty());
    bad = {{0, 0, -1, 4}};
    assert(LayerShape::recognize(bad).empty());
    std::vector<LayerShape::Rect> excessive(LayerShape::MAX_RECTS + 1, {0, 0, 10, 10});
    assert(LayerShape::recognize(excessive).empty());
    excessive.clear();
    for (int i = 0; i <= LayerShape::MAX_SHAPES; ++i) excessive.push_back({0, double(i * 20), 10, double(i * 20 + 10)});
    assert(LayerShape::recognize(excessive).empty());
    std::cout << "PASS: " << checks << " rounded region/stack fixtures, holes, notches, invalid geometry and bounded input\n";
}
