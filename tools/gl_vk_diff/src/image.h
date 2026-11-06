#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gvd {

// 统一的像素容器。约定：px 为 RGBA8，第 0 行 = 画面底部（GL 约定）。
// Vulkan 读回时是自上而下，读回函数会负责翻转，保证两边同向。
struct Image {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;

    void Alloc(int width, int height);
    bool Empty() const { return w <= 0 || h <= 0 || px.empty(); }
    uint8_t* At(int x, int y) { return px.data() + ((size_t)y * w + x) * 4; }
    const uint8_t* At(int x, int y) const { return px.data() + ((size_t)y * w + x) * 4; }
    void FlipVertical();
    uint64_t HashRGB() const;  // FNV-1a 64，帧缓冲哈希
};

bool SaveBMP(const std::string& path, const Image& img);

struct DiffStats {
    int w = 0, h = 0;
    double meanAbsRGB = 0.0;
    int maxRGB = 0;
    int overCount = 0;       // 任一 RGB 通道差 > threshold 的像素数
    double pctOver = 0.0;    // overCount / 总像素 * 100
    double meanAbsA = 0.0;
    int maxA = 0;
    uint64_t hashRef = 0;
    uint64_t hashTest = 0;
};

DiffStats Compare(const Image& ref, const Image& test, int threshold);

// 把 |差值| 放大 4 倍写成灰度 BMP，方便肉眼定位。
bool SaveDiffImage(const std::string& path, const Image& a, const Image& b);

}  // namespace gvd
