#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gvd {

// 计数器：只统计「工装自己能看见」的调用次数，不做任何驱动内部猜测。
struct Counters {
    uint64_t texUploads = 0;  // 纹理上传次数（TexImage2D 带数据）
    uint64_t texBytes = 0;    // 上传字节数
    uint64_t draws = 0;       // DrawArrays 调用次数（API 层 draw call）
    uint64_t quads = 0;       // 累计顶点数
    uint64_t presents = 0;    // 帧提交/呈现次数

    void Reset() { *this = Counters(); }
};

struct FrameStats {
    Counters c;
    std::vector<double> ms;  // 每帧耗时（毫秒），含端到端同步

    void Reset() {
        c.Reset();
        ms.clear();
    }
    void AddFrame(double v) { ms.push_back(v); }

    int Frames() const { return (int)ms.size(); }
    double Percentile(double p) const;
    double Avg() const;
    double Max() const;
    double Fps() const;
};

// 单行、可直接 grep 的摘要（不带换行）。
std::string PerfLine(const char* backend, int w, int h, const FrameStats& st);

}  // namespace gvd
