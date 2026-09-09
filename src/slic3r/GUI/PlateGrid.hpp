#ifndef slic3r_GUI_PlateGrid_hpp_
#define slic3r_GUI_PlateGrid_hpp_

// The plate grid rule, free of any GUI state.
//
// PartPlateList lays its plates out on a square-ish grid whose cell size is the bed
// size plus a fixed gap. Instances are stored in that grid's frame, so anything that
// needs to map an imported instance back to its plate has to reproduce the same rule.
// Keeping it here lets PartPlateList and the headless project reader share one
// definition instead of two that can drift apart.

#include <cmath>

#include "libslic3r/Point.hpp"

namespace Slic3r {

// Fraction of the bed size left between two neighbouring plates.
static constexpr double LOGICAL_PART_PLATE_GAP = 1. / 5.;

// Number of columns the grid uses for `count` plates.
inline int compute_colum_count(int count)
{
    float value = sqrt((float) count);
    float round_value = round(value);
    int cols;

    if (value > round_value)
        cols = round_value + 1;
    else
        cols = round_value;

    return cols;
}

// Distance between the origins of two horizontally/vertically adjacent plates.
inline double plate_stride_x(double plate_width) { return plate_width * (1. + LOGICAL_PART_PLATE_GAP); }
inline double plate_stride_y(double plate_depth) { return plate_depth * (1. + LOGICAL_PART_PLATE_GAP); }

// Origin of plate `index` in a grid of `cols` columns. Rows extend towards -Y.
inline Vec3d compute_plate_origin(int index, int cols, double plate_width, double plate_depth)
{
    const int row = index / cols;
    const int col = index % cols;
    return Vec3d(col * plate_stride_x(plate_width), -row * plate_stride_y(plate_depth), 0.);
}

} // namespace Slic3r

#endif /* slic3r_GUI_PlateGrid_hpp_ */
