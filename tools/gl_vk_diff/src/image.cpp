#include "image.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace gvd {

void Image::Alloc(int width, int height) {
    w = width;
    h = height;
    px.assign((size_t)w * h * 4, 0);
}

void Image::FlipVertical() {
    if (Empty()) return;
    std::vector<uint8_t> tmp((size_t)w * 4);
    for (int y = 0; y < h / 2; ++y) {
        uint8_t* a = At(0, y);
        uint8_t* b = At(0, h - 1 - y);
        std::memcpy(tmp.data(), a, tmp.size());
        std::memcpy(a, b, tmp.size());
        std::memcpy(b, tmp.data(), tmp.size());
    }
}

uint64_t Image::HashRGB() const {
    uint64_t h64 = 1469598103934665603ULL;  // FNV offset basis
    for (size_t i = 0; i + 3 < px.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            h64 ^= px[i + c];
            h64 *= 1099511628211ULL;
        }
    }
    return h64;
}

bool SaveBMP(const std::string& path, const Image& img) {
    if (img.Empty()) return false;
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;

    const uint32_t rowBytes = (uint32_t)img.w * 4;
    const uint32_t imgSize = rowBytes * (uint32_t)img.h;
    const uint32_t fileSize = 14 + 40 + imgSize;

    uint8_t fh[14] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0};
    std::memcpy(fh + 2, &fileSize, 4);

    uint8_t ih[40];
    std::memset(ih, 0, sizeof(ih));
    uint32_t v;
    v = 40;                          std::memcpy(ih + 0, &v, 4);
    v = (uint32_t)img.w;              std::memcpy(ih + 4, &v, 4);
    v = (uint32_t)img.h;              std::memcpy(ih + 8, &v, 4);  // 正值 = 自下而上，正好匹配我们的行序
    uint16_t v16 = 1;                 std::memcpy(ih + 12, &v16, 2);
    v16 = 32;                         std::memcpy(ih + 14, &v16, 2);
    v = imgSize;                      std::memcpy(ih + 20, &v, 4);
    uint32_t ppm = 2835;              std::memcpy(ih + 24, &ppm, 4);
                                      std::memcpy(ih + 28, &ppm, 4);

    fwrite(fh, 1, sizeof(fh), f);
    fwrite(ih, 1, sizeof(ih), f);

    std::vector<uint8_t> row(rowBytes);
    for (int y = 0; y < img.h; ++y) {
        const uint8_t* src = img.At(0, y);
        for (int x = 0; x < img.w; ++x) {
            row[x * 4 + 0] = src[x * 4 + 2];
            row[x * 4 + 1] = src[x * 4 + 1];
            row[x * 4 + 2] = src[x * 4 + 0];
            row[x * 4 + 3] = 0xFF;  // alpha 不写进可视图，避免看图软件把它当透明
        }
        fwrite(row.data(), 1, row.size(), f);
    }
    fclose(f);
    return true;
}

DiffStats Compare(const Image& ref, const Image& test, int threshold) {
    DiffStats st;
    if (ref.Empty() || test.Empty() || ref.w != test.w || ref.h != test.h) return st;
    st.w = ref.w;
    st.h = ref.h;
    st.hashRef = ref.HashRGB();
    st.hashTest = test.HashRGB();

    const int n = ref.w * ref.h;
    double sumRGB = 0.0, sumA = 0.0;
    for (int y = 0; y < ref.h; ++y) {
        const uint8_t* a = ref.At(0, y);
        const uint8_t* b = test.At(0, y);
        for (int x = 0; x < ref.w; ++x) {
            int dr = std::abs((int)a[x * 4 + 0] - (int)b[x * 4 + 0]);
            int dg = std::abs((int)a[x * 4 + 1] - (int)b[x * 4 + 1]);
            int db = std::abs((int)a[x * 4 + 2] - (int)b[x * 4 + 2]);
            int da = std::abs((int)a[x * 4 + 3] - (int)b[x * 4 + 3]);
            int m = std::max(dr, std::max(dg, db));
            sumRGB += (dr + dg + db) / 3.0;
            sumA += da;
            st.maxRGB = std::max(st.maxRGB, m);
            st.maxA = std::max(st.maxA, da);
            if (m > threshold) st.overCount++;
        }
    }
    st.meanAbsRGB = sumRGB / n;
    st.meanAbsA = sumA / n;
    st.pctOver = 100.0 * st.overCount / n;
    return st;
}

bool SaveDiffImage(const std::string& path, const Image& a, const Image& b) {
    if (a.Empty() || b.Empty() || a.w != b.w || a.h != b.h) return false;
    Image out;
    out.Alloc(a.w, a.h);
    for (int y = 0; y < a.h; ++y) {
        const uint8_t* pa = a.At(0, y);
        const uint8_t* pb = b.At(0, y);
        uint8_t* po = out.At(0, y);
        for (int x = 0; x < a.w; ++x) {
            int dr = std::abs((int)pa[x * 4 + 0] - (int)pb[x * 4 + 0]);
            int dg = std::abs((int)pa[x * 4 + 1] - (int)pb[x * 4 + 1]);
            int db = std::abs((int)pa[x * 4 + 2] - (int)pb[x * 4 + 2]);
            int m = std::max(dr, std::max(dg, db)) * 4;
            if (m > 255) m = 255;
            po[x * 4 + 0] = po[x * 4 + 1] = po[x * 4 + 2] = (uint8_t)m;
            po[x * 4 + 3] = 0xFF;
        }
    }
    return SaveBMP(path, out);
}

}  // namespace gvd
