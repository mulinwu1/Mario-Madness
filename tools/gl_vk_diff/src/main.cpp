// 验证工装入口。
//  diff  模式：同一批场景分别走 GL/ANGLE 与 Vulkan，输出像素差异报告与通过/失败判定
//  bench 模式：跑固定负载，输出一行可 grep 的性能基线（帧时间分布 / present / 上传 / draw call）
//
// 退出码：0 = 全部 PASS / UNSUPPORTED；1 = 存在 DIFF 或 ERROR；2 = 命令行错误。
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "backends.h"
#include "perf.h"
#include "scenes.h"

using namespace gvd;

namespace {

struct Options {
    std::string mode = "diff";
    int width = 512;
    int height = 512;
    int frames = 200;      // bench 模式帧数
    int quads = 512;       // bench 模式每帧四边形数
    int threshold = 8;     // 单像素任一 RGB 通道允许的最大绝对差
    double maxPct = 0.5;   // 允许的「超阈值像素」百分比
    bool runGl = true;
    std::string angleDir;
    std::string outDir = "out";
    std::vector<std::string> cases;  // 空 = 全部场景
    std::vector<int> vkModes;        // 0 = vk-direct，1 = vk-translate
};

void Usage() {
    printf(
        "用法: gl_vk_diff [diff|bench] [选项]\n"
        "\n"
        "  --mode=diff|bench    运行模式（默认 diff，也可作为第一个位置参数）\n"
        "  --w=N --h=N          渲染尺寸（默认 512x512）\n"
        "  --cases=a,b,c        只跑指定场景（默认全部）\n"
        "  --vk=direct,translate 选择 Vulkan 路径（默认两条都跑）\n"
        "  --no-gl              不跑 GL 参考（只产出 Vulkan 截图与哈希）\n"
        "  --angle-dir=DIR      含 libEGL.dll / libGLESv2.dll 的目录\n"
        "  --threshold=N        像素容差，默认 8\n"
        "  --max-pct=X          允许超阈值像素占比（%%），默认 0.5\n"
        "  --frames=N           bench 帧数，默认 200\n"
        "  --quads=N            bench 每帧四边形数，默认 512\n"
        "  --out=DIR            输出目录，默认 out\n");
}

std::string ToLower(std::string s) {
    for (size_t i = 0; i < s.size(); ++i) {
        s[i] = (char)tolower((unsigned char)s[i]);
    }
    return s;
}

bool StartsWith(const std::string& s, const char* prefix) {
    size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

std::string ValueOf(const std::string& arg) {
    size_t p = arg.find('=');
    return p == std::string::npos ? std::string() : arg.substr(p + 1);
}

std::vector<std::string> SplitComma(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        size_t p = s.find(',', start);
        std::string tok = (p == std::string::npos) ? s.substr(start) : s.substr(start, p - start);
        if (!tok.empty()) out.push_back(tok);
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

bool ParseArgs(int argc, char** argv, Options& o) {
    o.vkModes.push_back(0);
    o.vkModes.push_back(1);

    bool vkExplicit = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--help" || a == "-h") {
            Usage();
            return false;
        } else if (a == "--no-gl") {
            o.runGl = false;
        } else if (StartsWith(a, "--mode=")) {
            o.mode = ToLower(ValueOf(a));
        } else if (StartsWith(a, "--w=")) {
            o.width = std::atoi(ValueOf(a).c_str());
        } else if (StartsWith(a, "--h=")) {
            o.height = std::atoi(ValueOf(a).c_str());
        } else if (StartsWith(a, "--frames=")) {
            o.frames = std::atoi(ValueOf(a).c_str());
        } else if (StartsWith(a, "--quads=")) {
            o.quads = std::atoi(ValueOf(a).c_str());
        } else if (StartsWith(a, "--threshold=")) {
            o.threshold = std::atoi(ValueOf(a).c_str());
        } else if (StartsWith(a, "--max-pct=")) {
            o.maxPct = std::atof(ValueOf(a).c_str());
        } else if (StartsWith(a, "--cases=")) {
            o.cases = SplitComma(ValueOf(a));
        } else if (StartsWith(a, "--angle-dir=")) {
            o.angleDir = ValueOf(a);
        } else if (StartsWith(a, "--out=")) {
            o.outDir = ValueOf(a);
        } else if (StartsWith(a, "--vk=")) {
            std::vector<std::string> toks = SplitComma(ToLower(ValueOf(a)));
            o.vkModes.clear();
            for (size_t k = 0; k < toks.size(); ++k) {
                if (toks[k] == "direct" || toks[k] == "0") o.vkModes.push_back(0);
                else if (toks[k] == "translate" || toks[k] == "1") o.vkModes.push_back(1);
                else if (toks[k] == "both") { o.vkModes.push_back(0); o.vkModes.push_back(1); }
                else {
                    printf("未知的 --vk 取值: %s\n", toks[k].c_str());
                    return false;
                }
            }
            vkExplicit = true;
        } else if (a[0] == '-') {
            printf("未知参数: %s\n", a.c_str());
            Usage();
            return false;
        } else {
            o.mode = ToLower(a);
        }
    }

    if (!vkExplicit && o.vkModes.empty()) {
        o.vkModes.push_back(0);
        o.vkModes.push_back(1);
    }
    if (o.mode != "diff" && o.mode != "bench") {
        printf("未知模式: %s（只支持 diff / bench）\n", o.mode.c_str());
        return false;
    }
    if (o.width < 16 || o.height < 16) {
        printf("尺寸太小: %dx%d\n", o.width, o.height);
        return false;
    }
    if (o.frames < 1) o.frames = 1;
    if (o.quads < 2) o.quads = 2;
    return true;
}

std::string JoinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    char last = a[a.size() - 1];
    if (last == '\\' || last == '/') return a + b;
    return a + "\\" + b;
}

bool EnsureDir(const std::string& path) {
    if (path.empty()) return false;
    std::string cur;
    for (size_t i = 0; i < path.size(); ++i) {
        char c = path[i];
        cur += c;
        if ((c == '\\' || c == '/') && cur.size() > 3) {
            CreateDirectoryA(cur.c_str(), nullptr);
        }
    }
    CreateDirectoryA(path.c_str(), nullptr);
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::string BackendLabel(int mode) { return mode == 0 ? "vk-direct" : "vk-translate"; }

// ---------------------------------------------------------------- diff 模式

struct CaseResult {
    std::string scene;
    std::string backend;
    Status status = ST_ERROR;
    bool compared = false;       // 是否有可用的 GL 参考
    bool flippedBetter = false;  // 上下翻转后一致 → 说明只是 Y 轴方向反了
    std::string note;            // 场景自述（不支持原因等）
    std::string problem;         // 后端的错误信息
    DiffStats direct;
    DiffStats flipped;
    std::string bmpRef;
    std::string bmpTest;
    std::string bmpDiff;
    std::string hashRef;
};

std::string HashHex(uint64_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%016llx", (unsigned long long)v);
    return std::string(buf);
}

bool Passes(const DiffStats& s, const Options& o) {
    if (s.w <= 0 || s.h <= 0) return false;
    return s.pctOver <= o.maxPct;
}

// 跑一个场景并取回截图。supported=false 表示该后端跑不了这个场景（note 里写原因）。
bool RunScene(IGl& b, const Scene& sc, int w, int h, Image& out, bool& supported,
              std::string& note, std::string& problem) {
    supported = false;
    if (!b.BeginFrame(w, h, sc.clear)) {
        problem = "BeginFrame 失败";
        return false;
    }
    std::string n;
    supported = sc.run(b, w, h, n);
    note = n;
    bool ended = b.EndFrame();
    if (!supported) return false;
    if (!ended) {
        problem = "EndFrame 失败";
        if (problem.empty()) problem = b.LastProblem();
        return false;
    }
    out = b.Captured();
    if (out.Empty()) {
        problem = "截图为空";
        return false;
    }
    return true;
}

bool SceneSelected(const Options& o, const std::string& name) {
    if (o.cases.empty()) return true;
    for (size_t i = 0; i < o.cases.size(); ++i) {
        if (o.cases[i] == name) return true;
    }
    return false;
}

void WriteJsonReport(const std::string& path, const Options& o,
                     const std::vector<CaseResult>& results) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;
    fprintf(f, "{\n  \"width\": %d,\n  \"height\": %d,\n  \"threshold\": %d,\n  \"max_pct\": %.3f,\n  \"cases\": [\n",
            o.width, o.height, o.threshold, o.maxPct);
    for (size_t i = 0; i < results.size(); ++i) {
        const CaseResult& r = results[i];
        fprintf(f,
                "    {\"scene\": \"%s\", \"backend\": \"%s\", \"status\": \"%s\", \"compared\": %s, "
                "\"pct_over\": %.4f, \"max_rgb\": %d, \"mean_abs_rgb\": %.4f, "
                "\"flip_pct_over\": %.4f, \"hash_ref\": \"%s\", \"hash_test\": \"%s\", "
                "\"note\": \"%s\"}%s\n",
                r.scene.c_str(), r.backend.c_str(), r.compared ? StatusName(r.status) : "NO-REF",
                r.compared ? "true" : "false", r.direct.pctOver, r.direct.maxRGB,
                r.direct.meanAbsRGB, r.flipped.pctOver, r.hashRef.c_str(),
                HashHex(r.direct.hashTest).c_str(), r.note.c_str(),
                (i + 1 < results.size()) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
}

void WriteMarkdownReport(const std::string& path, const Options& o,
                         const std::vector<CaseResult>& results) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return;

    int nPass = 0, nDiff = 0, nUnsup = 0, nErr = 0, nNoRef = 0;
    for (size_t i = 0; i < results.size(); ++i) {
        if (!results[i].compared) { nNoRef++; continue; }
        switch (results[i].status) {
            case ST_PASS: nPass++; break;
            case ST_DIFF: nDiff++; break;
            case ST_UNSUPPORTED: nUnsup++; break;
            default: nErr++; break;
        }
    }

    fprintf(f, "# GL / Vulkan 一致性验证报告\n\n");
    fprintf(f, "- 渲染尺寸：%dx%d\n", o.width, o.height);
    fprintf(f, "- 判定阈值：单通道差 <= %d 视为一致；允许超阈值像素 %.2f%%\n", o.threshold, o.maxPct);
    fprintf(f, "- 共 %d 条用例：PASS %d / DIFF %d / UNSUPPORTED %d / ERROR %d",
            (int)results.size(), nPass, nDiff, nUnsup, nErr);
    if (nNoRef) fprintf(f, " / NO-REF %d", nNoRef);
    fprintf(f, "\n\n");

    fprintf(f, "## 汇总\n\n");
    fprintf(f, "| 场景 | 后端 | 结论 | 超阈值像素 | 最大通道差 | 平均通道差 | 说明 |\n");
    fprintf(f, "|---|---|---|---|---|---|---|\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const CaseResult& r = results[i];
        std::string verdict = r.compared ? StatusName(r.status) : "NO-REF";
        std::string desc = r.note;
        if (!r.compared && !r.problem.empty()) desc = r.problem;
        if (r.compared && r.status == ST_DIFF && r.flippedBetter)
            desc = "上下镜像：整幅图翻转后与参考一致，说明 Y 轴方向反了。" + desc;
        if (r.compared && r.status == ST_DIFF && !r.flippedBetter && desc.empty())
            desc = "像素不一致，详见对应 diff 图。";
        fprintf(f, "| %s | %s | %s | %.2f%% | %d | %.3f | %s |\n", r.scene.c_str(),
                r.backend.c_str(), verdict.c_str(), r.direct.pctOver, r.direct.maxRGB,
                r.direct.meanAbsRGB, desc.c_str());
    }

    fprintf(f, "\n## 明细\n\n");
    for (size_t i = 0; i < results.size(); ++i) {
        const CaseResult& r = results[i];
        const Scene* sc = FindScene(r.scene);
        fprintf(f, "### %s / %s\n\n", r.scene.c_str(), r.backend.c_str());
        if (sc) {
            fprintf(f, "- 覆盖语义：%s\n", sc->exercises);
            fprintf(f, "- 失败提示：%s\n", sc->hint);
        }
        if (!r.compared) {
            fprintf(f, "- 无 GL 参考像素（--no-gl，或 GL 后端初始化失败）：%s\n", r.problem.c_str());
        } else {
            fprintf(f, "- 参考哈希：%s\n", r.hashRef.c_str());
            fprintf(f, "- 本路哈希：%s\n", HashHex(r.direct.hashTest).c_str());
            fprintf(f, "- 直接比较：超阈值像素 %.2f%%，最大通道差 %d，平均通道差 %.3f\n",
                    r.direct.pctOver, r.direct.maxRGB, r.direct.meanAbsRGB);
            fprintf(f, "- 上下翻转后比较：超阈值像素 %.2f%%%s\n", r.flipped.pctOver,
                    r.flippedBetter ? "（翻转后一致 → Y 轴镜像）" : "");
        }
        if (!r.note.empty()) fprintf(f, "- 场景备注：%s\n", r.note.c_str());
        if (!r.problem.empty()) fprintf(f, "- 后端问题：%s\n", r.problem.c_str());
        if (!r.bmpRef.empty()) fprintf(f, "- 参考图：%s\n", r.bmpRef.c_str());
        if (!r.bmpTest.empty()) fprintf(f, "- 实测图：%s\n", r.bmpTest.c_str());
        if (!r.bmpDiff.empty()) fprintf(f, "- 差异图：%s\n", r.bmpDiff.c_str());
        fprintf(f, "\n");
    }
    fclose(f);
}

int RunDiffMode(const Options& o) {
    if (!EnsureDir(o.outDir)) {
        printf("无法创建输出目录: %s\n", o.outDir.c_str());
        return 2;
    }

    std::vector<CaseResult> results;
    std::string glProblem;
    IGl* gl = nullptr;
    if (o.runGl) {
        gl = CreateGlBackend(o.angleDir.empty() ? nullptr : o.angleDir.c_str(), glProblem);
        if (!gl) printf("[ref] GL/ANGLE 不可用：%s\n", glProblem.c_str());
    }

    for (int si = 0; si < SceneCount(); ++si) {
        const Scene& sc = SceneAt(si);
        if (!SceneSelected(o, sc.name)) continue;

        std::string sceneDir = JoinPath(o.outDir, sc.name);
        EnsureDir(sceneDir);

        // ---- 参考：GL/ANGLE ----
        Image ref;
        bool refOk = false;
        std::string refNote, refProblem;
        if (gl) {
            bool supported = false;
            refOk = RunScene(*gl, sc, o.width, o.height, ref, supported, refNote, refProblem);
            if (supported && !refOk && refProblem.empty()) refProblem = gl->LastProblem();
            if (!supported) refProblem = "GL 参考后端不支持该场景";
            if (refOk) {
                std::string p = JoinPath(sceneDir, "ref_gl.bmp");
                SaveBMP(p, ref);
            }
        }

        // ---- 被测：每条 Vulkan 路径 ----
        for (size_t m = 0; m < o.vkModes.size(); ++m) {
            int mode = o.vkModes[m];
            CaseResult r;
            r.scene = sc.name;
            r.backend = BackendLabel(mode);

            std::string problem;
            IGl* vk = CreateVkBackend(mode, o.width, o.height, problem);
            if (!vk) {
                r.status = ST_ERROR;
                r.problem = problem;
                results.push_back(r);
                printf("[%-12s] %-16s ERROR   %s\n", sc.name, r.backend.c_str(), problem.c_str());
                continue;
            }

            Image shot;
            bool supported = false;
            std::string note, runProblem;
            bool ran = RunScene(*vk, sc, o.width, o.height, shot, supported, note, runProblem);
            if (!ran && runProblem.empty()) runProblem = vk->LastProblem();
            r.note = note;
            r.problem = runProblem;

            if (!supported) {
                r.status = ST_UNSUPPORTED;
                results.push_back(r);
                printf("[%-12s] %-16s UNSUPPORTED %s\n", sc.name, r.backend.c_str(), note.c_str());
                delete vk;
                continue;
            }
            if (!ran) {
                r.status = ST_ERROR;
                results.push_back(r);
                printf("[%-12s] %-16s ERROR   %s\n", sc.name, r.backend.c_str(), runProblem.c_str());
                delete vk;
                continue;
            }

            std::string testPath = JoinPath(sceneDir, r.backend + ".bmp");
            SaveBMP(testPath, shot);
            r.bmpTest = testPath;
            r.direct.hashTest = shot.HashRGB();

            if (!refOk || ref.Empty()) {
                r.compared = false;
                r.status = ST_PASS;  // 不参与通过/失败判定，报告里显示 NO-REF
                r.problem = refProblem.empty() ? "没有 GL 参考像素" : refProblem;
                results.push_back(r);
                printf("[%-12s] %-16s NO-REF  %s\n", sc.name, r.backend.c_str(), r.bmpTest.c_str());
                delete vk;
                continue;
            }

            r.compared = true;
            r.direct = Compare(ref, shot, o.threshold);
            r.hashRef = HashHex(r.direct.hashRef);
            r.bmpRef = gl ? JoinPath(sceneDir, "ref_gl.bmp") : std::string();

            Image flipped = shot;
            flipped.FlipVertical();
            r.flipped = Compare(ref, flipped, o.threshold);

            bool pass = Passes(r.direct, o);
            r.flippedBetter = !pass && Passes(r.flipped, o);
            r.status = pass ? ST_PASS : ST_DIFF;

            if (!pass) {
                std::string dp = JoinPath(sceneDir, r.backend + ".diff.bmp");
                if (SaveDiffImage(dp, ref, shot)) r.bmpDiff = dp;
            }

            results.push_back(r);
            printf("[%-12s] %-16s %-6s over=%.2f%% max=%d mean=%.3f%s\n", sc.name, r.backend.c_str(),
                   StatusName(r.status), r.direct.pctOver, r.direct.maxRGB, r.direct.meanAbsRGB,
                   r.flippedBetter ? "  (Y 轴镜像)" : "");
            delete vk;
        }
    }

    delete gl;

    std::string mdPath = JoinPath(o.outDir, "report.md");
    std::string jsPath = JoinPath(o.outDir, "report.json");
    WriteMarkdownReport(mdPath, o, results);
    WriteJsonReport(jsPath, o, results);

    printf("\n报告: %s\n清单: %s\n", mdPath.c_str(), jsPath.c_str());

    bool failed = false;
    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i].compared && results[i].status == ST_DIFF) failed = true;
        if (results[i].status == ST_ERROR) failed = true;
    }
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------- bench 模式

std::vector<unsigned char> BenchTexture() {
    const int n = 64;
    std::vector<unsigned char> b((size_t)n * n * 4);
    for (int y = 0; y < n; ++y) {
        for (int x = 0; x < n; ++x) {
            int v = (((x / 8) + (y / 8)) & 1) ? 230 : 60;
            if (y < 4) v = 255;
            unsigned char* p = &b[((size_t)y * n + x) * 4];
            p[0] = (unsigned char)v;
            p[1] = (unsigned char)(v / 2);
            p[2] = (unsigned char)(255 - v);
            p[3] = 255;
        }
    }
    return b;
}

void BuildBenchVerts(std::vector<Vertex>& v, int quadCount, int w, int h) {
    v.clear();
    v.reserve((size_t)quadCount * 6);
    const int cols = 32;
    int rows = (quadCount + cols - 1) / cols;
    if (rows < 1) rows = 1;
    float cw = (float)w / (float)cols;
    float ch = (float)h / (float)rows;
    for (int i = 0; i < quadCount; ++i) {
        int cx = i % cols;
        int cy = i / cols;
        float x0 = cx * cw + 1.0f, y0 = cy * ch + 1.0f;
        float x1 = x0 + cw - 2.0f, y1 = y0 + ch - 2.0f;
        v.push_back({x0, y0, 0.0f, 0.0f});
        v.push_back({x1, y0, 1.0f, 0.0f});
        v.push_back({x1, y1, 1.0f, 1.0f});
        v.push_back({x0, y0, 0.0f, 0.0f});
        v.push_back({x1, y1, 1.0f, 1.0f});
        v.push_back({x0, y1, 0.0f, 1.0f});
    }
}

// 固定负载：每帧 2 个 draw call，共 quads 个四边形，1 张纹理。
bool RunBench(IGl& b, const Options& o, FrameStats& st, std::string& problem) {
    // 性能基线只测渲染，不做每帧读回；否则测出来的是 glReadPixels / 拷贝开销
    b.SetCaptureEnabled(false);
    Counters before = b.Perf();

    std::vector<unsigned char> tex = BenchTexture();
    int t = b.CreateTexture();
    b.BindTexture(t);
    b.TexImage2D(0, 64, 64, tex.data());

    std::vector<Vertex> verts;
    BuildBenchVerts(verts, o.quads, o.width, o.height);
    int total = (int)verts.size();
    int half = ((total / 2) / 6) * 6;
    if (half < 6) half = 6;
    if (half >= total) half = total - 6;

    int b1 = b.CreateBuffer();
    b.BindBuffer(b1);
    b.BufferData(verts.data(), (size_t)half * sizeof(Vertex));

    int b2 = b.CreateBuffer();
    b.BindBuffer(b2);
    b.BufferData(verts.data() + half, (size_t)(total - half) * sizeof(Vertex));

    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);

    float clear[4] = {0.05f, 0.05f, 0.10f, 1.0f};
    for (int f = 0; f < o.frames; ++f) {
        LARGE_INTEGER t0, t1;
        QueryPerformanceCounter(&t0);

        if (!b.BeginFrame(o.width, o.height, clear)) {
            problem = "BeginFrame 失败";
            return false;
        }
        b.BindTexture(t);

        b.BindBuffer(b1);
        b.VertexAttribPointer(0, 2, sizeof(Vertex), 0);
        b.EnableVertexAttribArray(0);
        b.VertexAttribPointer(1, 2, sizeof(Vertex), 8);
        b.EnableVertexAttribArray(1);
        b.DrawArrays(0, half);

        b.BindBuffer(b2);
        b.VertexAttribPointer(0, 2, sizeof(Vertex), 0);
        b.EnableVertexAttribArray(0);
        b.VertexAttribPointer(1, 2, sizeof(Vertex), 8);
        b.EnableVertexAttribArray(1);
        b.DrawArrays(0, total - half);

        if (!b.EndFrame()) {
            problem = "EndFrame 失败";
            return false;
        }

        QueryPerformanceCounter(&t1);
        double ms = 1000.0 * (double)(t1.QuadPart - t0.QuadPart) / (double)freq.QuadPart;
        st.AddFrame(ms);
    }

    Counters after = b.Perf();
    st.c.texUploads = after.texUploads - before.texUploads;
    st.c.texBytes = after.texBytes - before.texBytes;
    st.c.draws = after.draws - before.draws;
    st.c.quads = after.quads - before.quads;
    st.c.presents = after.presents - before.presents;
    return true;
}

int RunBenchMode(const Options& o) {
    if (!EnsureDir(o.outDir)) {
        printf("无法创建输出目录: %s\n", o.outDir.c_str());
        return 2;
    }

    printf("GVD_BENCH size=%dx%d frames=%d quads=%d draws_per_frame=2\n", o.width, o.height, o.frames,
           o.quads);

    std::vector<std::string> lines;
    int failures = 0;

    if (o.runGl) {
        std::string problem;
        IGl* gl = CreateGlBackend(o.angleDir.empty() ? nullptr : o.angleDir.c_str(), problem);
        if (!gl) {
            printf("[bench] GL/ANGLE 后端不可用：%s\n", problem.c_str());
            failures++;
        } else {
            FrameStats st;
            std::string p;
            if (RunBench(*gl, o, st, p)) {
                lines.push_back(PerfLine(gl->Name(), o.width, o.height, st));
            } else {
                printf("[bench] GL/ANGLE 运行失败：%s\n", p.c_str());
                failures++;
            }
            delete gl;
        }
    }

    for (size_t m = 0; m < o.vkModes.size(); ++m) {
        int mode = o.vkModes[m];
        std::string problem;
        IGl* vk = CreateVkBackend(mode, o.width, o.height, problem);
        if (!vk) {
            printf("[bench] %s 不可用：%s\n", BackendLabel(mode).c_str(), problem.c_str());
            failures++;
            continue;
        }
        FrameStats st;
        std::string p;
        if (RunBench(*vk, o, st, p)) {
            lines.push_back(PerfLine(vk->Name(), o.width, o.height, st));
        } else {
            printf("[bench] %s 运行失败：%s\n", BackendLabel(mode).c_str(), p.c_str());
            failures++;
        }
        delete vk;
    }

    for (size_t i = 0; i < lines.size(); ++i) printf("%s\n", lines[i].c_str());

    std::string path = JoinPath(o.outDir, "perf.log");
    FILE* f = fopen(path.c_str(), "wb");
    if (f) {
        fprintf(f, "GVD_BENCH size=%dx%d frames=%d quads=%d draws_per_frame=2\n", o.width, o.height,
                o.frames, o.quads);
        for (size_t i = 0; i < lines.size(); ++i) fprintf(f, "%s\n", lines[i].c_str());
        fclose(f);
        printf("\n基线: %s\n", path.c_str());
    }
    return failures ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!ParseArgs(argc, argv, o)) return 2;
    if (o.mode == "bench") return RunBenchMode(o);
    return RunDiffMode(o);
}
