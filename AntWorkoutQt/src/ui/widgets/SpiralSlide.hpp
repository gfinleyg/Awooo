#pragma once

// How the workout screen's coming up list moves as a block finishes (C++ only): each block traces a small outward
// spiral where it is, then slides to its new place, each one a little after the one above it. The list's rows
// (LongVLabels) and the block going up into the Up Next box (HudScreen) move by the same clock.

#include <QPointF>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace awo::spiralslide {

struct Motion {
    // the spiral (design px at its widest, turns clockwise, ms), then the slide (ms), and each block's wait after the
    // one above it starts (ms)
    double radius = 3.0;
    double turns = 2.5;
    int spiralMs = 350;
    int slideMs = 900;
    int staggerMs = 50;

    int duration(int cells) const { return std::max(cells - 1, 0) * staggerMs + spiralMs + slideMs; }
};

// a block finishing
inline constexpr Motion kBlockDone{};
// a scroll (wheel or drag): a quick glide, no spiral or stagger
inline constexpr Motion kScroll{0.0, 0.0, 0, 300, 0};

// where a block is ms into its own move (after its wait): its wobble off its path (px at the scale given), and how far
// along from where it was to where it goes (0 to 1, easing in and out)
struct Step {
    QPointF offset;
    double progress = 0.0;
};

inline QPointF spiralAt(const Motion& motion, double p, double scale) {
    // (from straight up, clockwise on screen, opening out to its widest)
    const double angle = -std::numbers::pi / 2 + 2 * std::numbers::pi * motion.turns * p;
    const double r = motion.radius * scale * p;
    return {r * std::cos(angle), r * std::sin(angle)};
}

inline double easeInOut(double q) {
    return q < 0.5 ? 4 * q * q * q : 1 - std::pow(-2 * q + 2, 3) / 2;
}

inline Step at(const Motion& motion, double ms, double scale) {
    if (ms <= 0) {
        return {};
    }
    if (ms < motion.spiralMs) {
        return {spiralAt(motion, ms / motion.spiralMs, scale), 0.0};
    }
    const double q = motion.slideMs > 0 ? std::min(1.0, (ms - motion.spiralMs) / motion.slideMs) : 1.0;
    const double e = easeInOut(q);
    // (the spiral's end eased away over the slide)
    return {spiralAt(motion, 1.0, scale) * (1 - e), e};
}

}  // namespace awo::spiralslide
