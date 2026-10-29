// Static: this translation unit's copy of stb_image_write (the JPEG preview) does not clash
// with the examples'.
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "runtime/image.hpp"

#include "core/log.hpp"
#include "render/frame/preview_extent.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>
#include <thread>

namespace ohao::runtime {

namespace {

// For one axis of an area resample: each output index's source span [first, first + n)
// and the weight of each source index in it (the covered part, divided by the scale).
struct Taps {
    std::vector<uint32_t> first;
    std::vector<uint32_t> count;
    std::vector<float> weights;    // concatenated, count[i] per output i
    std::vector<uint32_t> offset;  // into weights
};

Taps areaTaps(uint32_t src, uint32_t dst) {
    Taps t;
    const double scale = static_cast<double>(src) / dst;
    for (uint32_t i = 0; i < dst; ++i) {
        const double a = i * scale, b = (i + 1) * scale;
        const uint32_t lo = static_cast<uint32_t>(std::floor(a));
        const uint32_t hi = std::min(src, static_cast<uint32_t>(std::ceil(b)));
        t.first.push_back(lo);
        t.count.push_back(hi - lo);
        t.offset.push_back(static_cast<uint32_t>(t.weights.size()));
        for (uint32_t k = lo; k < hi; ++k) {
            const double cover = std::min<double>(b, k + 1) - std::max<double>(a, k);
            t.weights.push_back(static_cast<float>(cover / scale));
        }
    }
    return t;
}

void appendBytes(void* ctx, void* data, int size) {
    auto* out = static_cast<std::string*>(ctx);
    out->append(static_cast<const char*>(data), static_cast<std::size_t>(size));
}

// ---- PNG: a fast lossless encoder for captures ---------------------------------------------
// stb_image_write's PNG took 7.3 s for one 1280x720 Cornell frame (its deflate tries up to
// 16 earlier positions per byte, twice with lazy matching), against a 30 ms budget for the
// whole capture. This writer filters Up (row 0: Sub), then deflates with the fixed Huffman
// codes, a byte repeated three or more times being a distance-1 match; four bands of rows
// are encoded on separate threads. Flat rows filter to runs and compress well; a band of
// noise is stored instead when that is smaller.

struct FixedCodes {
    uint16_t lit[288];      // bit-reversed fixed Huffman codes (deflate writes LSB first)
    uint8_t litBits[288];
    uint16_t lenSym[259];   // match length -> symbol (257..285), extra bits and their value
    uint8_t lenExtraBits[259];
    uint16_t lenExtra[259];
    uint32_t match[259];    // a whole distance-1 match of each length: symbol, extra bits, distance 0
    uint8_t matchBits[259];
    uint32_t crc[8][256];   // slicing-by-8 tables
};

uint32_t reverseBits(uint32_t code, int bits) {
    uint32_t r = 0;
    for (int i = 0; i < bits; ++i) r |= ((code >> i) & 1u) << (bits - 1 - i);
    return r;
}

const FixedCodes& fixedCodes() {
    static const FixedCodes t = [] {
        FixedCodes c{};
        for (int s = 0; s < 288; ++s) {
            int bits, code;
            if (s <= 143) { bits = 8; code = 0x30 + s; }
            else if (s <= 255) { bits = 9; code = 0x190 + (s - 144); }
            else if (s <= 279) { bits = 7; code = s - 256; }
            else { bits = 8; code = 0xC0 + (s - 280); }
            c.lit[s] = static_cast<uint16_t>(reverseBits(static_cast<uint32_t>(code), bits));
            c.litBits[s] = static_cast<uint8_t>(bits);
        }
        static constexpr uint16_t base[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
                                            31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static constexpr uint8_t extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                            2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        for (int len = 3; len <= 258; ++len) {
            int k = 28;
            while (base[k] > len) --k;
            c.lenSym[len] = static_cast<uint16_t>(257 + k);
            c.lenExtraBits[len] = extra[k];
            c.lenExtra[len] = static_cast<uint16_t>(len - base[k]);
        }
        for (int len = 3; len <= 258; ++len) {   // symbol, then extra bits, then distance code 0 (5 bits)
            const int sym = c.lenSym[len];
            c.match[len] = c.lit[sym] | (static_cast<uint32_t>(c.lenExtra[len]) << c.litBits[sym]);
            c.matchBits[len] = static_cast<uint8_t>(c.litBits[sym] + c.lenExtraBits[len] + 5);
        }
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t v = n;
            for (int k = 0; k < 8; ++k) v = (v & 1u) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
            c.crc[0][n] = v;
        }
        for (uint32_t n = 0; n < 256; ++n)
            for (int s = 1; s < 8; ++s) c.crc[s][n] = (c.crc[s - 1][n] >> 8) ^ c.crc[0][c.crc[s - 1][n] & 0xFFu];
        return c;
    }();
    return t;
}

// CRC-32 (PNG's), eight bytes a step.
uint32_t crc32(const uint8_t* p, std::size_t n, uint32_t crc = 0) {
    const auto& t = fixedCodes().crc;
    crc = ~crc;
    for (; n >= 8; n -= 8, p += 8) {
        const uint32_t lo = (static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 |
                             static_cast<uint32_t>(p[2]) << 16 | static_cast<uint32_t>(p[3]) << 24) ^ crc;
        const uint32_t hi = static_cast<uint32_t>(p[4]) | static_cast<uint32_t>(p[5]) << 8 |
                            static_cast<uint32_t>(p[6]) << 16 | static_cast<uint32_t>(p[7]) << 24;
        crc = t[7][lo & 0xFFu] ^ t[6][(lo >> 8) & 0xFFu] ^ t[5][(lo >> 16) & 0xFFu] ^ t[4][lo >> 24] ^
              t[3][hi & 0xFFu] ^ t[2][(hi >> 8) & 0xFFu] ^ t[1][(hi >> 16) & 0xFFu] ^ t[0][hi >> 24];
    }
    for (; n > 0; --n, ++p) crc = t[0][(crc ^ *p) & 0xFFu] ^ (crc >> 8);
    return ~crc;
}

uint32_t adler32(const uint8_t* p, std::size_t n) {
    uint32_t a = 1, b = 0;
    while (n > 0) {
        const std::size_t block = std::min<std::size_t>(n, 5552);   // no overflow before the modulo
        for (std::size_t i = 0; i < block; ++i) {
            a += p[i];
            b += a;
        }
        a %= 65521u;
        b %= 65521u;
        p += block;
        n -= block;
    }
    return (b << 16) | a;
}

void putU32(std::string& s, uint32_t v) {
    for (int k = 3; k >= 0; --k) s.push_back(static_cast<char>((v >> (8 * k)) & 0xFFu));
}

void putChunk(std::string& s, const char type[4], const uint8_t* data, std::size_t n) {
    putU32(s, static_cast<uint32_t>(n));
    const std::size_t typeAt = s.size();
    s.append(type, 4);
    s.append(reinterpret_cast<const char*>(data), n);
    putU32(s, crc32(reinterpret_cast<const uint8_t*>(s.data()) + typeAt, n + 4));
}

// zlib's adler32_combine: the checksum of A then B from A's, B's and B's length.
uint32_t adlerCombine(uint32_t a1, uint32_t a2, std::size_t len2) {
    constexpr uint64_t kBase = 65521;
    const uint64_t rem = len2 % kBase;
    uint64_t sum1 = a1 & 0xFFFFu;
    uint64_t sum2 = (rem * sum1) % kBase;
    sum1 += (a2 & 0xFFFFu) + kBase - 1;
    sum2 += ((a1 >> 16) & 0xFFFFu) + ((a2 >> 16) & 0xFFFFu) + kBase - rem;
    if (sum1 >= kBase) sum1 -= kBase;
    if (sum1 >= kBase) sum1 -= kBase;
    if (sum2 >= kBase << 1) sum2 -= kBase << 1;
    if (sum2 >= kBase) sum2 -= kBase;
    return static_cast<uint32_t>(sum1 | (sum2 << 16));
}

// One band of rows: filtered, then deflated into byte-aligned blocks of its own, so bands
// are compressed on separate threads and concatenated (pigz's way). The last band's last
// block is final; any other band ends with an empty stored block, which aligns it.
struct Band {
    std::unique_ptr<uint8_t[]> f;   // filtered scanlines
    std::unique_ptr<uint8_t[]> z;   // deflate blocks
    std::size_t n{0}, zn{0};
    uint32_t adler{1};
};

void encodeBand(Band& band, std::span<const uint8_t> pixels, std::size_t row, uint32_t y0, uint32_t y1, bool last) {
    const FixedCodes& c = fixedCodes();
    // Filter: a filter byte, then the row (Up: minus the row above; the image's first row:
    // Sub). Both buffers are written before they are read: no zero fill.
    const std::size_t n = static_cast<std::size_t>(y1 - y0) * (row + 1);
    band.n = n;
    band.f.reset(new uint8_t[n]);
    for (uint32_t y = y0; y < y1; ++y) {
        uint8_t* dst = band.f.get() + (y - y0) * (row + 1);
        const uint8_t* cur = pixels.data() + y * row;
        if (y == 0) {
            dst[0] = 1;   // Sub
            for (std::size_t i = 0; i < row; ++i) dst[1 + i] = static_cast<uint8_t>(cur[i] - (i >= 4 ? cur[i - 4] : 0));
        } else {
            dst[0] = 2;   // Up
            const uint8_t* up = cur - row;
            for (std::size_t i = 0; i < row; ++i) dst[1 + i] = static_cast<uint8_t>(cur[i] - up[i]);
        }
    }
    const uint8_t* d = band.f.get();
    band.adler = adler32(d, n);

    // One fixed-Huffman block: at most 9 bits a byte, plus slack (and room for the stored
    // form below). The bit writer's state lives in locals: stores through a byte pointer may
    // alias anything, so members would be reloaded after every byte written.
    band.z.reset(new uint8_t[n * 9 / 8 + 64]);
    uint8_t* o = band.z.get();
    uint64_t buf = (last ? 1u : 0u) | (1u << 1);   // BFINAL, then BTYPE = 01 (fixed codes)
    int count = 3;
    const auto put = [&](uint32_t code, int bits) {
        buf |= static_cast<uint64_t>(code) << count;
        count += bits;
        if (count >= 32) {
            const uint32_t word = static_cast<uint32_t>(buf);
            std::memcpy(o, &word, 4);   // little-endian: deflate's bit order
            o += 4;
            buf >>= 32;
            count -= 32;
        }
    };
    std::size_t i = 0;
    while (i < n) {
        const uint8_t b = d[i++];
        put(c.lit[b], c.litBits[b]);
        std::size_t run = 0;
        const std::size_t limit = std::min<std::size_t>(258, n - i);
        while (run < limit && d[i + run] == b) ++run;
        if (run >= 3) {   // a match at distance 1: the byte again, `run` times
            put(c.match[run], c.matchBits[run]);
            i += run;
        }
    }
    put(c.lit[256], c.litBits[256]);   // end of block
    if (!last) put(0, 3);              // an empty stored block (BFINAL 0, BTYPE 00): aligns the band
    while (count > 0) {
        *o++ = static_cast<uint8_t>(buf);
        buf >>= 8;
        count -= 8;
    }
    if (!last) {   // the empty stored block's LEN and NLEN
        const uint8_t empty[4] = {0x00, 0x00, 0xFF, 0xFF};
        std::memcpy(o, empty, 4);
        o += 4;
    }
    band.zn = static_cast<std::size_t>(o - band.z.get());
    // Noise (a one-frame RT GI render is mostly noise) codes at ~8.5 bits a byte: then the
    // stored form (raw bytes in blocks of 65535, 5 header bytes each) is smaller.
    const std::size_t blocks = (n + 65534) / 65535;
    if (band.zn > n + blocks * 5) {
        o = band.z.get();
        for (std::size_t at = 0; at < n; at += 65535) {
            const uint16_t len = static_cast<uint16_t>(std::min<std::size_t>(65535, n - at));
            *o++ = last && at + len >= n ? 1 : 0;   // BFINAL on the very last; BTYPE 00 (stored)
            const uint16_t nlen = static_cast<uint16_t>(~len);
            std::memcpy(o, &len, 2);
            std::memcpy(o + 2, &nlen, 2);
            std::memcpy(o + 4, d + at, len);
            o += 4 + len;
        }
        band.zn = static_cast<std::size_t>(o - band.z.get());
    }
}

std::string pngOf(std::span<const uint8_t> pixels, uint32_t width, uint32_t height) {
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    // A fixed band count (not the machine's core count), so the file's bytes depend on the
    // image alone.
    const uint32_t bands = std::max<uint32_t>(1, std::min<uint32_t>(4, height / 64));
    std::vector<Band> parts(bands);
    std::vector<std::thread> workers;
    for (uint32_t k = 0; k < bands; ++k) {
        const uint32_t y0 = height * k / bands, y1 = height * (k + 1) / bands;
        const bool last = k + 1 == bands;
        if (k + 1 < bands)
            workers.emplace_back([&, k, y0, y1, last] { encodeBand(parts[k], pixels, row, y0, y1, last); });
        else
            encodeBand(parts[k], pixels, row, y0, y1, last);
    }
    for (auto& t : workers) t.join();

    // zlib: header, the bands' blocks, then the Adler-32 of the whole filtered stream.
    std::size_t total = 2 + 4;
    for (const Band& b : parts) total += b.zn;
    const std::unique_ptr<uint8_t[]> z(new uint8_t[total]);
    z[0] = 0x78;
    z[1] = 0x01;
    std::size_t zn = 2;
    uint32_t ad = parts[0].adler;
    for (uint32_t k = 0; k < bands; ++k) {
        std::memcpy(z.get() + zn, parts[k].z.get(), parts[k].zn);
        zn += parts[k].zn;
        if (k > 0) ad = adlerCombine(ad, parts[k].adler, parts[k].n);
    }
    for (int k = 3; k >= 0; --k) z[zn++] = static_cast<uint8_t>((ad >> (8 * k)) & 0xFFu);

    std::string png;
    png.reserve(zn + 64);
    static constexpr uint8_t kSignature[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    png.append(reinterpret_cast<const char*>(kSignature), sizeof kSignature);
    uint8_t ihdr[13];
    for (int k = 0; k < 4; ++k) {
        ihdr[k] = static_cast<uint8_t>(width >> (24 - 8 * k));
        ihdr[4 + k] = static_cast<uint8_t>(height >> (24 - 8 * k));
    }
    ihdr[8] = 8;    // bits per channel
    ihdr[9] = 6;    // RGBA
    ihdr[10] = 0;   // deflate
    ihdr[11] = 0;   // adaptive filtering
    ihdr[12] = 0;   // no interlace
    putChunk(png, "IHDR", ihdr, sizeof ihdr);
    putChunk(png, "IDAT", z.get(), zn);
    putChunk(png, "IEND", nullptr, 0);
    return png;
}

} // namespace

Rgba8Image crop(std::span<const uint8_t> frame, uint32_t width, uint32_t height, const std::array<int32_t, 4>& r) {
    (void)height;
    Rgba8Image out;
    out.width = static_cast<uint32_t>(r[2]);
    out.height = static_cast<uint32_t>(r[3]);
    out.pixels.resize(static_cast<std::size_t>(out.width) * out.height * 4);
    const std::size_t rowBytes = static_cast<std::size_t>(out.width) * 4;
    for (uint32_t y = 0; y < out.height; ++y) {
        const std::size_t src = (static_cast<std::size_t>(r[1] + y) * width + static_cast<std::size_t>(r[0])) * 4;
        std::copy_n(frame.data() + src, rowBytes, out.pixels.data() + y * rowBytes);
    }
    return out;
}

Rgba8Image fitWithin(Rgba8Image image, uint32_t maxSide) {
    const auto [w, h] = render::previewExtent(image.width, image.height, maxSide);
    if (w == image.width && h == image.height) return image;
    const Taps tx = areaTaps(image.width, w), ty = areaTaps(image.height, h);

    // Vertical pass first: each output row is a weighted sum of whole source rows, one
    // contiguous multiply-add per tap. Then horizontal, over only the h rows left.
    const std::size_t inRow = static_cast<std::size_t>(image.width) * 4, outRow = static_cast<std::size_t>(w) * 4;
    std::vector<float> acc(inRow);
    Rgba8Image out;
    out.width = w;
    out.height = h;
    out.pixels.resize(outRow * h);
    const uint32_t* first = tx.first.data();
    const uint32_t* count = tx.count.data();
    const uint32_t* offset = tx.offset.data();
    const float* weights = tx.weights.data();
    for (uint32_t y = 0; y < h; ++y) {
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (uint32_t k = 0; k < ty.count[y]; ++k) {
            const float wgt = ty.weights[ty.offset[y] + k];
            const uint8_t* src = image.pixels.data() + (ty.first[y] + k) * inRow;
            for (std::size_t i = 0; i < inRow; ++i) acc[i] += wgt * static_cast<float>(src[i]);
        }
        uint8_t* dst = out.pixels.data() + y * outRow;
        for (uint32_t x = 0; x < w; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            const float* p = acc.data() + static_cast<std::size_t>(first[x]) * 4;
            const float* wt = weights + offset[x];
            for (uint32_t k = 0; k < count[x]; ++k, p += 4) {
                r += wt[k] * p[0];
                g += wt[k] * p[1];
                b += wt[k] * p[2];
                a += wt[k] * p[3];
            }
            dst[x * 4 + 0] = static_cast<uint8_t>(std::min(r + 0.5f, 255.0f));
            dst[x * 4 + 1] = static_cast<uint8_t>(std::min(g + 0.5f, 255.0f));
            dst[x * 4 + 2] = static_cast<uint8_t>(std::min(b + 0.5f, 255.0f));
            dst[x * 4 + 3] = static_cast<uint8_t>(std::min(a + 0.5f, 255.0f));
        }
    }
    return out;
}

std::string encodePng(const Rgba8Image& image) { return pngOf(image.pixels, image.width, image.height); }

std::string encodeJpeg(const Rgba8Image& image, int quality) {
    std::string out;
    stbi_write_jpg_to_func(appendBytes, &out, static_cast<int>(image.width), static_cast<int>(image.height), 4,
                           image.pixels.data(), quality);
    return out;
}

bool writePng(const std::string& path, std::span<const uint8_t> pixels, uint32_t width, uint32_t height) {
    const auto t0 = std::chrono::steady_clock::now();
    const std::string png = pngOf(pixels, width, height);
    const auto t1 = std::chrono::steady_clock::now();
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f.write(png.data(), static_cast<std::streamsize>(png.size()));
    f.close();
    OHAO_LOG(Debug, render, "png: encode {:.2f} ms ({} bytes), write {:.2f} ms",
             std::chrono::duration<double, std::milli>(t1 - t0).count(), png.size(),
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
    return !f.fail();
}

bool writePfm(const std::string& path, std::span<const float> rgba, uint32_t width, uint32_t height) {
    if (rgba.size() != static_cast<std::size_t>(width) * height * 4) return false;
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    const std::string header = "PF\n" + std::to_string(width) + " " + std::to_string(height) + "\n-1.0\n";
    f.write(header.data(), static_cast<std::streamsize>(header.size()));
    std::vector<float> row(static_cast<std::size_t>(width) * 3);
    for (uint32_t y = height; y-- > 0;) {
        const float* src = rgba.data() + static_cast<std::size_t>(y) * width * 4;
        for (uint32_t x = 0; x < width; ++x) {
            row[x * 3 + 0] = src[x * 4 + 0];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 2];
        }
        f.write(reinterpret_cast<const char*>(row.data()), static_cast<std::streamsize>(row.size() * sizeof(float)));
    }
    f.close();
    return !f.fail();
}

} // namespace ohao::runtime
