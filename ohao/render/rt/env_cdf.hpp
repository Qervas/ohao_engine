#pragma once

// Env CDF — CPU-side 2D importance-sampling CDF builder for HDR environment maps.
//
// Given equirectangular float pixels, builds:
//   - marginalCDF[y]      cumulative distribution over rows, weighted by sin(theta)
//   - conditionalCDF[y*W+x]  per-row cumulative distribution over columns
//
// The sin(theta) weight accounts for the sphere's Jacobian: polar rows represent
// less solid angle than equatorial rows. integral() returns the total luminance
// integral (pre-normalization), used on the GPU to normalize sample PDFs.
//
// Usage:
//   EnvCDF cdf;
//   cdf.build(hdrPixels, width, height);
//   upload cdf.marginalCDF(), cdf.conditionalCDF() to GPU storage buffers

#include <vector>

namespace ohao {

// Builds 2D importance-sampling CDFs for an HDR environment map in equirectangular layout.
//
// After build(), call marginalCDF() / conditionalCDF() to access the upload-ready data:
//   marginalCDF[y] in [0,1]      — CDF over rows (y index), weighted by sin(theta)
//   conditionalCDF[y*W + x] in [0,1]  — CDF over columns within row y
//
// integral() returns the total luminance integral (used for PDF normalization on GPU).
class EnvCDF {
public:
    // pixels: RGBA float, row-major, width*height*4 floats. Alpha ignored.
    void build(const float* pixels, int width, int height);

    int width() const { return m_width; }
    int height() const { return m_height; }
    float integral() const { return m_integral; }

    const std::vector<float>& marginalCDF() const { return m_marginalCDF; }
    const std::vector<float>& conditionalCDF() const { return m_conditionalCDF; }

private:
    int m_width = 0;
    int m_height = 0;
    float m_integral = 0.0f;
    std::vector<float> m_marginalCDF;       // size = height
    std::vector<float> m_conditionalCDF;    // size = width * height
};

} // namespace ohao
