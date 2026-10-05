#pragma once

// RGB565 blending that survives both byte orders. The device framebuffer is
// panel order (EYES_RGB565_SWAPPED); the host stays native. Anything that
// composites over an already-encoded pixel goes through here, never through
// a hand-rolled shift on the raw word.

#include <cstdint>

#include "eyes/raster.hpp"

namespace eyes {

inline std::uint16_t pixel_unpack(std::uint16_t encoded)
{
    return kSwapBytes ? static_cast<std::uint16_t>((encoded << 8) | (encoded >> 8)) : encoded;
}

inline std::uint16_t pixel_pack(std::uint16_t native)
{
    return kSwapBytes ? static_cast<std::uint16_t>((native << 8) | (native >> 8)) : native;
}

// under and over are encoded (framebuffer order); amount 0 keeps under.
inline std::uint16_t blend565(std::uint16_t under, std::uint16_t over, float amount)
{
    const std::uint16_t u = pixel_unpack(under);
    const std::uint16_t o = pixel_unpack(over);
    const int ur = (u >> 11) & 31;
    const int ug = (u >> 5) & 63;
    const int ub = u & 31;
    const int red = ur + static_cast<int>(static_cast<float>(((o >> 11) & 31) - ur) * amount);
    const int green = ug + static_cast<int>(static_cast<float>(((o >> 5) & 63) - ug) * amount);
    const int blue = ub + static_cast<int>(static_cast<float>((o & 31) - ub) * amount);
    return pixel_pack(static_cast<std::uint16_t>((red << 11) | (green << 5) | blue));
}

// The 5-bit red channel of an encoded pixel: for white-on-black ink it is the
// coverage already on the glass, so a new stamp can max-merge against it
// without a coverage buffer.
inline int pixel_red5(std::uint16_t encoded) { return (pixel_unpack(encoded) >> 11) & 31; }

// Encoded white at a coverage in [0, 1] (gray, all channels).
inline std::uint16_t white_at(float coverage)
{
    const int level = static_cast<int>(coverage * 31.0F + 0.5F);
    const int level6 = static_cast<int>(coverage * 63.0F + 0.5F);
    return pixel_pack(static_cast<std::uint16_t>((level << 11) | (level6 << 5) | level));
}

}  // namespace eyes
