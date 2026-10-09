// The spell radial: which of the six spell elements the right stick points at (see docs/ARCHITECTURE.md, "Spell radial").
#pragma once

namespace ring {

constexpr int kSectors = 6;

// The radial's layout, in fractions of the height it is sized by: from the centre to a holder's centre, a holder's width,
// and the backdrop disc's radius (it just holds the holders).
constexpr float kRadius = 0.27f;
constexpr float kHolderWidth = 0.22f;
constexpr float kDiscRadius = kRadius + kHolderWidth * 0.7f;

// How the six sectors sit: Sides has one at the right and one at the left (and two above, two below); Top has one at the
// top and one at the bottom, so three sit above the middle and three below (red, blue, yellow left to right on top).
enum class Orientation { Sides, Top };

struct State {
    bool open = false;  // the stick is pushed far enough that the radial is up
    int sector = -1;    // the sector it points at while open (0 = right, then counter-clockwise every 60 degrees)
};

// The radial's state for the right stick (x to the right, y down, each -1 to 1) and its state a moment ago. It opens when the
// stick is pushed past `open_at` and closes below `close_at` (so a stick wobbling at the edge does not flicker); the sector
// changes only once the stick is `margin` degrees into the next one (so it does not chatter on a boundary).
State pick(float x, float y, const State& previous, Orientation orientation = Orientation::Sides, float open_at = 0.6f,
           float close_at = 0.4f, float margin = 8.0f);

// The spell element (0 to 5: the gems of the bottom screen from top to bottom: red, blue, yellow, green, orange, teal) of a
// sector. The elements run clockwise from the upper right, the order of the gems going down the screen.
int element_of(int sector, Orientation orientation = Orientation::Sides);

// A sector's centre as an angle in degrees, 0 = right, counter-clockwise (screen up is 90). The sectors are numbered
// counter-clockwise from the first at 0 degrees (Sides) or 30 degrees (Top).
float sector_angle(int sector, Orientation orientation = Orientation::Sides);

}  // namespace ring
