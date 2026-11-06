#include "perf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace gvd {

double FrameStats::Percentile(double p) const {
    if (ms.empty()) return 0.0;
    std::vector<double> v = ms;
    std::sort(v.begin(), v.end());
    double idx = p * (double)(v.size() - 1);
    int lo = (int)std::floor(idx);
    int hi = (int)std::ceil(idx);
    if (lo < 0) lo = 0;
    if (hi >= (int)v.size()) hi = (int)v.size() - 1;
    double t = idx - lo;
    return v[lo] * (1.0 - t) + v[hi] * t;
}

double FrameStats::Avg() const {
    if (ms.empty()) return 0.0;
    double s = 0;
    for (double v : ms) s += v;
    return s / ms.size();
}

double FrameStats::Max() const {
    if (ms.empty()) return 0.0;
    return *std::max_element(ms.begin(), ms.end());
}

double FrameStats::Fps() const {
    double a = Avg();
    return a > 0.0 ? 1000.0 / a : 0.0;
}

std::string PerfLine(const char* backend, int w, int h, const FrameStats& st) {
    char buf[512];
    snprintf(buf, sizeof(buf),
             "GVD_PERF backend=%s size=%dx%d frames=%d avg_ms=%.3f p50_ms=%.3f p95_ms=%.3f "
             "max_ms=%.3f fps=%.1f present=%llu tex_uploads=%llu tex_bytes=%llu "
             "draw_calls=%llu verts=%llu",
             backend, w, h, st.Frames(), st.Avg(), st.Percentile(0.50), st.Percentile(0.95),
             st.Max(), st.Fps(), (unsigned long long)st.c.presents,
             (unsigned long long)st.c.texUploads, (unsigned long long)st.c.texBytes,
             (unsigned long long)st.c.draws, (unsigned long long)st.c.quads);
    return std::string(buf);
}

}  // namespace gvd
