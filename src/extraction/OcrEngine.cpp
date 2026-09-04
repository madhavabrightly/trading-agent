#include "extraction/OcrEngine.hpp"
#include "logger.hpp"

#include <windows.h>
#include <gdiplus.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <mutex>

#pragma comment(lib, "gdiplus.lib")

// ONNX Runtime C API (dynamically loaded at runtime — MinGW-safe, no .lib).
#include "onnxruntime_c_api.h"

namespace edgemon {

std::string OcrPageResult::fullText() const {
    std::ostringstream oss;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) oss << "\n";
        oss << lines[i].text;
    }
    return oss.str();
}

// ============================================================================
// Bitmap font — kept ONLY for the renderText test hook (not used for OCR).
// ============================================================================
static const uint8_t FONT5x7[95][7] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, {0x04,0x04,0x04,0x04,0x04,0x00,0x04},
    {0x0A,0x0A,0x0A,0x00,0x00,0x00,0x00}, {0x0A,0x0A,0x1F,0x0A,0x1F,0x0A,0x0A},
    {0x04,0x0E,0x14,0x0E,0x05,0x0E,0x04}, {0x18,0x19,0x02,0x04,0x08,0x13,0x03},
    {0x0C,0x12,0x14,0x08,0x15,0x12,0x0D}, {0x0C,0x04,0x08,0x00,0x00,0x00,0x00},
    {0x02,0x04,0x08,0x08,0x08,0x04,0x02}, {0x08,0x04,0x02,0x02,0x02,0x04,0x08},
    {0x00,0x04,0x15,0x0E,0x15,0x04,0x00}, {0x00,0x04,0x04,0x1F,0x04,0x04,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x04,0x08}, {0x00,0x00,0x00,0x1F,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, {0x01,0x02,0x04,0x08,0x10,0x00,0x00},
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C},
    {0x00,0x0C,0x0C,0x00,0x0C,0x0C,0x00}, {0x00,0x0C,0x0C,0x00,0x0C,0x04,0x08},
    {0x02,0x04,0x08,0x10,0x08,0x04,0x02}, {0x00,0x00,0x1F,0x00,0x1F,0x00,0x00},
    {0x08,0x04,0x02,0x01,0x02,0x04,0x08}, {0x0E,0x11,0x01,0x02,0x04,0x00,0x04},
    {0x0E,0x11,0x13,0x15,0x15,0x10,0x0E}, {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11},
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E},
    {0x1E,0x09,0x09,0x09,0x09,0x09,0x1E}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F},
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F},
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E},
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, {0x11,0x12,0x14,0x18,0x14,0x12,0x11},
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, {0x11,0x1B,0x15,0x15,0x11,0x11,0x11},
    {0x11,0x19,0x15,0x13,0x11,0x11,0x11}, {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D},
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E},
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, {0x11,0x11,0x11,0x11,0x11,0x11,0x0E},
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, {0x11,0x11,0x11,0x15,0x15,0x15,0x0A},
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, {0x11,0x11,0x0A,0x04,0x04,0x04,0x04},
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, {0x0E,0x08,0x08,0x08,0x08,0x08,0x0E},
    {0x10,0x08,0x04,0x02,0x01,0x00,0x00}, {0x0E,0x02,0x02,0x02,0x02,0x02,0x0E},
    {0x04,0x0A,0x11,0x00,0x00,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00,0x00,0x1F},
    {0x08,0x04,0x02,0x00,0x00,0x00,0x00}, {0x00,0x00,0x0E,0x01,0x0F,0x11,0x0F},
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x1E}, {0x00,0x00,0x0E,0x11,0x10,0x11,0x0E},
    {0x01,0x01,0x0F,0x11,0x11,0x11,0x0F}, {0x00,0x00,0x0E,0x11,0x1F,0x10,0x0E},
    {0x06,0x09,0x08,0x1C,0x08,0x08,0x08}, {0x00,0x0F,0x11,0x11,0x0F,0x01,0x0E},
    {0x10,0x10,0x1E,0x11,0x11,0x11,0x11}, {0x04,0x00,0x0C,0x04,0x04,0x04,0x0E},
    {0x02,0x00,0x06,0x02,0x02,0x12,0x0C}, {0x10,0x10,0x12,0x14,0x18,0x14,0x12},
    {0x0C,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x00,0x00,0x1A,0x15,0x15,0x15,0x15},
    {0x00,0x00,0x1E,0x11,0x11,0x11,0x11}, {0x00,0x00,0x0E,0x11,0x11,0x11,0x0E},
    {0x00,0x1E,0x11,0x11,0x1E,0x10,0x10}, {0x00,0x0F,0x11,0x11,0x0F,0x01,0x01},
    {0x00,0x00,0x16,0x19,0x10,0x10,0x10}, {0x00,0x00,0x0F,0x10,0x0E,0x01,0x1E},
    {0x08,0x08,0x1C,0x08,0x08,0x09,0x06}, {0x00,0x00,0x11,0x11,0x11,0x13,0x0D},
    {0x00,0x00,0x11,0x11,0x11,0x0A,0x04}, {0x00,0x00,0x11,0x11,0x15,0x15,0x0A},
    {0x00,0x00,0x11,0x0A,0x04,0x0A,0x11}, {0x00,0x11,0x11,0x0F,0x01,0x01,0x0E},
    {0x00,0x00,0x1F,0x02,0x04,0x08,0x1F}, {0x02,0x04,0x04,0x08,0x04,0x04,0x02},
    {0x04,0x04,0x04,0x04,0x04,0x04,0x04}, {0x08,0x04,0x04,0x02,0x04,0x04,0x08},
    {0x00,0x00,0x0A,0x15,0x00,0x00,0x00}
};

// ============================================================================
// ONNX Runtime dynamic loader (C API, MinGW-safe)
// ============================================================================

struct OrtRuntime {
    HMODULE dll = nullptr;
    const OrtApi* api = nullptr;

    bool load() {
        if (api) return true;
        std::vector<std::wstring> candidates;
        wchar_t exeDir[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) {
            std::wstring d(exeDir);
            auto slash = d.find_last_of(L"\\/");
            if (slash != std::wstring::npos) {
                d = d.substr(0, slash);
                candidates.push_back(d + L"\\onnxruntime.dll");
                candidates.push_back(d + L"\\..\\lib\\onnxruntime.dll");
            }
        }
        candidates.push_back(L"onnxruntime.dll");

        for (const auto& c : candidates) {
            dll = LoadLibraryW(c.c_str());
            if (dll) break;
        }
        if (!dll) return false;

        auto getBase = reinterpret_cast<const OrtApiBase* (*)(void)>(
            GetProcAddress(dll, "OrtGetApiBase"));
        if (!getBase) { FreeLibrary(dll); dll = nullptr; return false; }
        const OrtApiBase* base = getBase();
        api = base ? base->GetApi(ORT_API_VERSION) : nullptr;
        if (!api) { FreeLibrary(dll); dll = nullptr; return false; }
        return true;
    }

    ~OrtRuntime() { if (dll) FreeLibrary(dll); }
};

// ============================================================================
// PIMPL
// ============================================================================

struct OcrEngine::Impl {
    OrtRuntime ort;
    OrtEnv* env = nullptr;
    OrtSession* detSession = nullptr;
    OrtSession* recSession = nullptr;
    OrtMemoryInfo* memInfo = nullptr;

    std::string detInputName;
    std::string detOutputName;
    std::string recInputName;
    std::string recOutputName;
    std::vector<std::string> dict;
    std::mutex runMutex;

    bool loaded = false;
    std::string loadError;

    ~Impl() {
        if (ort.api) {
            if (detSession) ort.api->ReleaseSession(detSession);
            if (recSession) ort.api->ReleaseSession(recSession);
            if (env) ort.api->ReleaseEnv(env);
            if (memInfo) ort.api->ReleaseMemoryInfo(memInfo);
        }
    }
};

// ============================================================================
// Construction / model loading
// ============================================================================

OcrEngine::OcrEngine() : impl_(std::make_unique<Impl>()) {}

OcrEngine::~OcrEngine() = default;

static bool readIoName(const OrtApi* api, OrtSession* s, bool isInput,
                       int index, std::string& out, std::string& err) {
    char* name = nullptr;
    OrtAllocator* alloc = nullptr;
    OrtStatus* ast = api->GetAllocatorWithDefaultOptions(&alloc);
    if (ast) { err = api->GetErrorMessage(ast); api->ReleaseStatus(ast); return false; }
    OrtStatus* st = isInput
        ? api->SessionGetInputName(s, static_cast<size_t>(index), alloc, &name)
        : api->SessionGetOutputName(s, static_cast<size_t>(index), alloc, &name);
    if (st) { err = api->GetErrorMessage(st); api->ReleaseStatus(st); return false; }
    out = name ? name : "";
    if (name) {
        OrtStatus* fst = api->AllocatorFree(alloc, name);
        if (fst) { api->ReleaseStatus(fst); }
    }
    return true;
}

static std::vector<std::string> loadDictFile(const std::string& path) {
    std::vector<std::string> dict;
    std::ifstream f(path);
    if (!f) return dict;
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        dict.push_back(line);
    }
    return dict;
}

bool OcrEngine::loadModels(const std::string& modelsDir) {
    auto& I = *impl_;
    if (I.loaded) return true;

    auto fail = [&](const std::string& why) {
        error_ = why;
        I.loadError = why;
        LOG_ERROR("OcrEngine: model load failed: {}", why);
        return false;
    };

    if (!I.ort.load()) {
        return fail("onnxruntime.dll not found (place it next to edge_monitor.exe or in lib/)");
    }
    const OrtApi* api = I.ort.api;

    OrtStatus* st = api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "edgemon_ocr", &I.env);
    if (st) { std::string e = api->GetErrorMessage(st); api->ReleaseStatus(st); return fail("CreateEnv: " + e); }

    st = api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &I.memInfo);
    if (st) { std::string e = api->GetErrorMessage(st); api->ReleaseStatus(st); return fail("CreateCpuMemoryInfo: " + e); }

    I.dict = loadDictFile(modelsDir + "/en_dict.txt");
    if (I.dict.empty()) {
        return fail("dict not found at " + modelsDir + "/en_dict.txt");
    }

    std::string detPath = modelsDir + "/en_PP-OCRv3_det.onnx";
    std::string recPath = modelsDir + "/en_PP-OCRv3_rec.onnx";
    {
        std::ifstream f(detPath, std::ios::binary);
        if (!f) return fail("det model not found at " + detPath);
    }
    {
        std::ifstream f(recPath, std::ios::binary);
        if (!f) return fail("rec model not found at " + recPath);
    }

    OrtSessionOptions* sopts = nullptr;
    st = api->CreateSessionOptions(&sopts);
    if (!st) st = api->SetSessionGraphOptimizationLevel(sopts, ORT_ENABLE_ALL);
    if (!st) st = api->SetIntraOpNumThreads(sopts, 2);
    if (st) { std::string e = api->GetErrorMessage(st); api->ReleaseStatus(st); return fail("session options: " + e); }

    // ORTCHAR_T is wchar_t on Windows: convert paths to wide strings.
    std::wstring detPathW(detPath.begin(), detPath.end());
    std::wstring recPathW(recPath.begin(), recPath.end());

    st = api->CreateSession(I.env, detPathW.c_str(), sopts, &I.detSession);
    if (st) {
        std::string e = api->GetErrorMessage(st);
        api->ReleaseStatus(st);
        api->ReleaseSessionOptions(sopts);
        return fail("det model load: " + e);
    }
    st = api->CreateSession(I.env, recPathW.c_str(), sopts, &I.recSession);
    if (st) {
        std::string e = api->GetErrorMessage(st);
        api->ReleaseStatus(st);
        api->ReleaseSessionOptions(sopts);
        return fail("rec model load: " + e);
    }
    api->ReleaseSessionOptions(sopts);

    std::string err;
    if (!readIoName(api, I.detSession, true, 0, I.detInputName, err)) return fail("det input name: " + err);
    if (!readIoName(api, I.detSession, false, 0, I.detOutputName, err)) return fail("det output name: " + err);
    if (!readIoName(api, I.recSession, true, 0, I.recInputName, err)) return fail("rec input name: " + err);
    if (!readIoName(api, I.recSession, false, 0, I.recOutputName, err)) return fail("rec output name: " + err);

    I.loaded = true;
    LOG_INFO("OcrEngine: PP-OCRv3 loaded (det input '{}' -> '{}'; rec input '{}' -> '{}'; dict {} chars)",
             I.detInputName, I.detOutputName, I.recInputName, I.recOutputName, I.dict.size());
    return true;
}

bool OcrEngine::available() const {
    return impl_ && impl_->loaded && impl_->ort.api &&
           impl_->detSession && impl_->recSession;
}

// ============================================================================
// Image helpers
// ============================================================================

static void bgraToGray(const uint8_t* bgra, int w, int h, std::vector<float>& gray) {
    gray.resize(static_cast<size_t>(w) * h);
    for (int i = 0; i < w * h; ++i) {
        const uint8_t* p = bgra + static_cast<size_t>(i) * 4;
        gray[static_cast<size_t>(i)] = static_cast<float>(
            (static_cast<int>(p[2]) * 299 + static_cast<int>(p[1]) * 587 +
             static_cast<int>(p[0]) * 114) / 1000);
    }
}

static std::vector<float> resizeGray(const std::vector<float>& src, int sw, int sh,
                                     int dw, int dh) {
    std::vector<float> dst(static_cast<size_t>(dw) * dh, 255.0f);
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return dst;
    float sx = static_cast<float>(sw) / dw;
    float sy = static_cast<float>(sh) / dh;
    for (int y = 0; y < dh; ++y) {
        float fy = (static_cast<float>(y) + 0.5f) * sy - 0.5f;
        int y0 = static_cast<int>(std::floor(fy));
        fy -= static_cast<float>(y0);
        y0 = std::max(0, std::min(y0, sh - 1));
        int y1 = std::min(y0 + 1, sh - 1);
        for (int x = 0; x < dw; ++x) {
            float fx = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
            int x0 = static_cast<int>(std::floor(fx));
            fx -= static_cast<float>(x0);
            x0 = std::max(0, std::min(x0, sw - 1));
            int x1 = std::min(x0 + 1, sw - 1);
            float v = src[static_cast<size_t>(y0) * sw + x0] * (1 - fx) * (1 - fy)
                    + src[static_cast<size_t>(y0) * sw + x1] * fx * (1 - fy)
                    + src[static_cast<size_t>(y1) * sw + x0] * (1 - fx) * fy
                    + src[static_cast<size_t>(y1) * sw + x1] * fx * fy;
            dst[static_cast<size_t>(y) * dw + x] = v;
        }
    }
    return dst;
}

static std::vector<float> grayToNchw(const std::vector<float>& gray, int w, int h) {
    std::vector<float> out(static_cast<size_t>(3) * w * h);
    for (int i = 0; i < w * h; ++i) {
        float v = (gray[static_cast<size_t>(i)] / 255.0f - 0.5f) / 0.5f;
        out[static_cast<size_t>(i)] = v;
        out[static_cast<size_t>(w * h) + i] = v;
        out[static_cast<size_t>(2) * w * h + i] = v;
    }
    return out;
}

// ============================================================================
// Detector post-processing (probability map -> text boxes)
// ============================================================================

struct DetBox { int x0, y0, x1, y1; float score; };

static std::vector<DetBox> probMapToBoxes(const float* prob, int w, int h,
                                          float thresh = 0.3f, int minSide = 4) {
    std::vector<DetBox> boxes;
    if (!prob || w <= 0 || h <= 0) return boxes;

    // Downsample the mask scan for very large maps (stride 2 keeps speed sane
    // and still captures text lines).
    std::vector<uint8_t> mask(static_cast<size_t>(w) * h, 0);
    for (int i = 0; i < w * h; ++i) {
        mask[static_cast<size_t>(i)] = prob[i] > thresh ? 1 : 0;
    }

    std::vector<int> stack;
    stack.reserve(w * h / 4);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            size_t idx = static_cast<size_t>(y) * w + x;
            if (!mask[idx]) continue;
            int x0 = x, x1 = x, y0 = y, y1 = y;
            float sumScore = 0;
            int cnt = 0;
            mask[idx] = 0;
            stack.clear();
            stack.push_back(static_cast<int>(idx));
            while (!stack.empty()) {
                int cur = stack.back();
                stack.pop_back();
                int cx = cur % w;
                int cy = cur / w;
                x0 = std::min(x0, cx); x1 = std::max(x1, cx);
                y0 = std::min(y0, cy); y1 = std::max(y1, cy);
                sumScore += prob[cur];
                cnt++;
                if (cx > 0     && mask[static_cast<size_t>(cy) * w + cx - 1]) { mask[static_cast<size_t>(cy) * w + cx - 1] = 0; stack.push_back(static_cast<int>(cy) * w + cx - 1); }
                if (cx < w - 1 && mask[static_cast<size_t>(cy) * w + cx + 1]) { mask[static_cast<size_t>(cy) * w + cx + 1] = 0; stack.push_back(static_cast<int>(cy) * w + cx + 1); }
                if (cy > 0     && mask[static_cast<size_t>(cy - 1) * w + cx]) { mask[static_cast<size_t>(cy - 1) * w + cx] = 0; stack.push_back(static_cast<int>(cy - 1) * w + cx); }
                if (cy < h - 1 && mask[static_cast<size_t>(cy + 1) * w + cx]) { mask[static_cast<size_t>(cy + 1) * w + cx] = 0; stack.push_back(static_cast<int>(cy + 1) * w + cx); }
            }
            int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
            if (bw < minSide || bh < minSide) continue;
            DetBox b;
            b.x0 = x0; b.y0 = y0; b.x1 = x1; b.y1 = y1;
            b.score = cnt > 0 ? sumScore / cnt : 0.0f;
            boxes.push_back(b);
        }
    }
    return boxes;
}

// ============================================================================
// Recognition (CTC decode)
// ============================================================================

struct RecResult { std::string text; float confidence = 0.0f; };

static RecResult ctcDecode(const float* data, int timesteps, int numClasses,
                           const std::vector<std::string>& dict) {
    RecResult r;
    if (!data || timesteps <= 0 || numClasses <= 0) return r;
    // Empirically calibrated for this PP-OCRv3 export:
    //  - class 0 is the CTC blank (the model emits 0 at non-text frames),
    //  - classes 1..N map to dict[0..N-1] (offset 1). A correct crop decoded
    //    with offset 0 yields cleanly ASCII-shifted text ("Dpoufout" for
    //    "Contents"), proving the +1 offset.
    int blank = 0;
    int dictOffset = 1;

    std::string text;
    int prev = -1;
    float confSum = 0;
    int confN = 0;
    for (int t = 0; t < timesteps; ++t) {
        const float* row = data + static_cast<size_t>(t) * numClasses;
        int best = 0;
        float bestV = row[0];
        for (int c = 1; c < numClasses; ++c) {
            if (row[c] > bestV) { bestV = row[c]; best = c; }
        }
        if (best == blank || best == prev) continue;
        prev = best;
        int dictIdx = best - dictOffset;
        if (dictIdx >= 0 && dictIdx < static_cast<int>(dict.size())) {
            text += dict[static_cast<size_t>(dictIdx)];
            confSum += bestV;
            confN++;
        }
    }
    r.text = text;
    r.confidence = confN > 0 ? confSum / confN : 0.0f;
    return r;
}

// Runs a gray crop through the rec model (48px tall, up to 320 wide).
static RecResult recognizeCrop(const OrtApi* api, OrtSession* session,
                               OrtMemoryInfo* mem,
                               const std::vector<float>& grayCrop, int cw, int ch,
                               const std::string& inName, const std::string& outName,
                               const std::vector<std::string>& dict, std::string* errOut) {
    RecResult r;
    constexpr int recH = 48;
    constexpr int recW = 320;

    auto resized = resizeGray(grayCrop, cw, ch, recW, recH);
    auto input = grayToNchw(resized, recW, recH);

    std::array<int64_t, 4> shape = {1, 3, recH, recW};
    OrtValue* inTensor = nullptr;
    OrtStatus* st = api->CreateTensorWithDataAsOrtValue(
        mem, input.data(), input.size() * sizeof(float), shape.data(), 4,
        ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &inTensor);
    if (st) { if (errOut) *errOut = api->GetErrorMessage(st); api->ReleaseStatus(st); return r; }

    const char* inNames[] = {inName.c_str()};
    const char* outNames[] = {outName.c_str()};
    OrtValue* outTensor = nullptr;
    st = api->Run(session, nullptr, inNames, &inTensor, 1, outNames, 1, &outTensor);
    api->ReleaseValue(inTensor);
    if (st) {
        if (errOut) *errOut = api->GetErrorMessage(st);
        api->ReleaseStatus(st);
        return r;
    }

    float* outData = nullptr;
    st = api->GetTensorMutableData(outTensor, reinterpret_cast<void**>(&outData));
    if (!st) {
        OrtTensorTypeAndShapeInfo* info = nullptr;
        if (!api->GetTensorTypeAndShape(outTensor, &info)) {
            size_t rank = 0;
            OrtStatus* cst = api->GetDimensionsCount(info, &rank);
            if (!cst && (rank == 2 || rank == 3)) {
                int64_t dims[3] = {0, 0, 0};
                OrtStatus* dst = api->GetDimensions(info, dims, rank);
                if (!dst) {
                    int T = 0, C = 0;
                    if (rank == 3) { T = static_cast<int>(dims[1]); C = static_cast<int>(dims[2]); }
                    else if (rank == 2) { T = static_cast<int>(dims[0]); C = static_cast<int>(dims[1]); }
                    if (T > 0 && C > 0) {
                        r = ctcDecode(outData, T, C, dict);
                    }
                } else {
                    api->ReleaseStatus(dst);
                }
            } else if (cst) {
                api->ReleaseStatus(cst);
            }
            api->ReleaseTensorTypeAndShapeInfo(info);
        }
    }
    api->ReleaseValue(outTensor);
    return r;
}

// ============================================================================
// PNG decode via GDI+ (kept; used by decodePng / recognizePngBase64)
// ============================================================================

static bool decodePngData(const std::vector<uint8_t>& png,
                          std::vector<uint8_t>& pixels,
                          int& width, int& height) {
#ifdef _WIN32
    if (png.size() < 8) return false;
    // GDI+ startup/shutdown is NOT thread-safe; multiple OCR paths (watcher
    // thread, OCRManager thread) can decode concurrently. Serialize all PNG
    // decoding through one mutex.
    static std::mutex gdiplusMutex;
    std::lock_guard<std::mutex> lock(gdiplusMutex);

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken = 0;
    if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, nullptr) != Gdiplus::Ok) {
        return false;
    }
    bool ok = false;
    {
        IStream* stream = nullptr;
        if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) == S_OK) {
            ULONG written = 0;
            stream->Write(png.data(), static_cast<ULONG>(png.size()), &written);
            LARGE_INTEGER pos{};
            stream->Seek(pos, STREAM_SEEK_SET, nullptr);
            Gdiplus::Bitmap bitmap(stream, FALSE);
            if (bitmap.GetLastStatus() == Gdiplus::Ok) {
                width = static_cast<int>(bitmap.GetWidth());
                height = static_cast<int>(bitmap.GetHeight());
                pixels.assign(static_cast<size_t>(width) * height * 4, 0);
                Gdiplus::Rect rect(0, 0, width, height);
                Gdiplus::BitmapData data;
                if (bitmap.LockBits(&rect, Gdiplus::ImageLockModeRead,
                                    PixelFormat32bppARGB, &data) == Gdiplus::Ok) {
                    const uint8_t* src = static_cast<const uint8_t*>(data.Scan0);
                    for (int y = 0; y < height; ++y) {
                        std::memcpy(pixels.data() + static_cast<size_t>(y) * width * 4,
                                    src + static_cast<size_t>(y) * data.Stride,
                                    static_cast<size_t>(width) * 4);
                    }
                    bitmap.UnlockBits(&data);
                    ok = true;
                }
            }
            stream->Release();
        }
    }
    Gdiplus::GdiplusShutdown(gdiplusToken);
    return ok;
#else
    (void)png; (void)pixels; (void)width; (void)height;
    return false;
#endif
}

bool OcrEngine::decodePng(const std::vector<uint8_t>& pngBytes,
                          std::vector<uint8_t>& pixels,
                          int& width, int& height) {
    return decodePngData(pngBytes, pixels, width, height);
}

bool OcrEngine::decodePngBase64(const std::string& base64,
                                std::vector<uint8_t>& pixels,
                                int& width, int& height) {
    if (base64.empty()) return false;
    static const std::string b64Chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> png;
    int val = 0, bits = 0;
    for (char c : base64) {
        if (c == '=') break;
        auto pos = b64Chars.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) | static_cast<int>(pos);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            png.push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
        }
    }
    if (png.size() < 8) return false;
    return decodePngData(png, pixels, width, height);
}

// ============================================================================
// recognize()
// ============================================================================

OcrPageResult OcrEngine::recognize(const uint8_t* bgra, int width, int height) {
    OcrPageResult result;
    if (!bgra || width <= 0 || height <= 0) return result;
    if (!available()) {
        LOG_WARN("OcrEngine: recognize() called but engine unavailable: {}",
                 error_.empty() ? "models not loaded" : error_);
        return result;
    }

    auto& I = *impl_;
    const OrtApi* api = I.ort.api;
    std::lock_guard<std::mutex> lock(I.runMutex);

    auto t0 = std::chrono::steady_clock::now();

    std::vector<float> gray;
    bgraToGray(bgra, width, height, gray);

    // --- Detection pass ---
    const int maxSide = 960;
    float scale = 1.0f;
    int detW = width, detH = height;
    if (std::max(width, height) > maxSide) {
        scale = static_cast<float>(maxSide) / std::max(width, height);
        detW = static_cast<int>(width * scale) & ~31;
        detH = static_cast<int>(height * scale) & ~31;
        if (detW < 32) detW = 32;
        if (detH < 32) detH = 32;
    }
    std::vector<float> detGray = (scale == 1.0f)
        ? gray : resizeGray(gray, width, height, detW, detH);
    auto detInput = grayToNchw(detGray, detW, detH);
    std::array<int64_t, 4> detShape = {1, 3, detH, detW};
    LOG_DEBUG("OcrEngine: recognize() step det-input {}x{} (img {}x{})",
              detW, detH, width, height);

    OrtValue* detIn = nullptr;
    OrtStatus* st = api->CreateTensorWithDataAsOrtValue(
        I.memInfo, detInput.data(), detInput.size() * sizeof(float),
        detShape.data(), 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &detIn);
    if (st) {
        LOG_WARN("OcrEngine: det input alloc failed: {}", api->GetErrorMessage(st));
        api->ReleaseStatus(st);
        return result;
    }
    const char* detInNames[] = {I.detInputName.c_str()};
    const char* detOutNames[] = {I.detOutputName.c_str()};
    OrtValue* detOut = nullptr;
    LOG_DEBUG("OcrEngine: running det model...");
    st = api->Run(I.detSession, nullptr, detInNames, &detIn, 1, detOutNames, 1, &detOut);
    api->ReleaseValue(detIn);
    LOG_DEBUG("OcrEngine: det model done");
    if (st) {
        LOG_WARN("OcrEngine: det run failed: {}", api->GetErrorMessage(st));
        api->ReleaseStatus(st);
        return result;
    }

    float* prob = nullptr;
    st = api->GetTensorMutableData(detOut, reinterpret_cast<void**>(&prob));
    if (st || !prob) {
        if (st) api->ReleaseStatus(st);
        api->ReleaseValue(detOut);
        return result;
    }

    // Read the REAL output shape. PP-OCRv3 DB outputs a probability map at 1/4
    // resolution of the input (feature-map stride), e.g. input 960x960 ->
    // output 240x240. Reading detW*detH floats would overrun the buffer.
    int mapW = detW, mapH = detH;
    {
        OrtTensorTypeAndShapeInfo* info = nullptr;
        if (!api->GetTensorTypeAndShape(detOut, &info)) {
            size_t rank = 0;
            OrtStatus* cst = api->GetDimensionsCount(info, &rank);
            if (!cst && rank >= 2) {
                int64_t dims[4] = {0, 0, 0, 0};
                OrtStatus* dst = api->GetDimensions(info, dims, rank);
                if (!dst) {
                    // [1,1,H,W] or [1,H,W] -> last two dims are H,W.
                    mapH = static_cast<int>(dims[rank - 2]);
                    mapW = static_cast<int>(dims[rank - 1]);
                } else {
                    api->ReleaseStatus(dst);
                }
            } else if (cst) {
                api->ReleaseStatus(cst);
            }
            api->ReleaseTensorTypeAndShapeInfo(info);
        }
    }
    LOG_DEBUG("OcrEngine: det output map {}x{} (input {}x{})", mapW, mapH, detW, detH);
    if (mapW <= 0 || mapH <= 0) { api->ReleaseValue(detOut); return result; }

    std::vector<DetBox> boxes = probMapToBoxes(prob, mapW, mapH);
    api->ReleaseValue(detOut);

    // Scale boxes from map space back to full-res image space.
    float xScale = static_cast<float>(width) / mapW;
    float yScale = static_cast<float>(height) / mapH;

    // Merge vertically-overlapping fragments into text-line boxes.
    std::vector<DetBox> merged;
    for (auto& b : boxes) {
        DetBox fb;
        fb.x0 = static_cast<int>(b.x0 * xScale);
        fb.y0 = static_cast<int>(b.y0 * yScale);
        fb.x1 = static_cast<int>((b.x1 + 1) * xScale) - 1;
        fb.y1 = static_cast<int>((b.y1 + 1) * yScale) - 1;
        fb.x0 = std::max(0, fb.x0); fb.y0 = std::max(0, fb.y0);
        fb.x1 = std::min(width - 1, fb.x1); fb.y1 = std::min(height - 1, fb.y1);
        fb.score = b.score;
        bool joined = false;
        for (auto& m : merged) {
            int overlapY = std::min(m.y1, fb.y1) - std::max(m.y0, fb.y0);
            int hA = m.y1 - m.y0 + 1, hB = fb.y1 - fb.y0 + 1;
            int gapX = fb.x0 - m.x1;
            if (overlapY > std::min(hA, hB) / 2 && gapX < std::max(hA, hB) * 3) {
                m.x0 = std::min(m.x0, fb.x0);
                m.y0 = std::min(m.y0, fb.y0);
                m.x1 = std::max(m.x1, fb.x1);
                m.y1 = std::max(m.y1, fb.y1);
                m.score = std::max(m.score, fb.score);
                joined = true;
                break;
            }
        }
        if (!joined) merged.push_back(fb);
    }

    std::sort(merged.begin(), merged.end(), [](const DetBox& a, const DetBox& b) {
        if (a.y0 != b.y0) return a.y0 < b.y0;
        return a.x0 < b.x0;
    });

    // --- Recognition pass ---
    std::string lastErr;
    for (auto& b : merged) {
        int bw = b.x1 - b.x0 + 1, bh = b.y1 - b.y0 + 1;
        if (bw < 4 || bh < 4) continue;
        // Expand the box a little so glyph ascenders/descenders are included.
        int pad = std::max(1, bh / 8);
        int cx0 = std::max(0, b.x0 - pad);
        int cy0 = std::max(0, b.y0 - pad / 2);
        int cx1 = std::min(width - 1, b.x1 + pad);
        int cy1 = std::min(height - 1, b.y1 + pad / 2);
        bw = cx1 - cx0 + 1;
        bh = cy1 - cy0 + 1;
        if (bw < 4 || bh < 4) continue;

        std::vector<float> crop(static_cast<size_t>(bw) * bh);
        for (int y = 0; y < bh; ++y) {
            std::memcpy(crop.data() + static_cast<size_t>(y) * bw,
                        gray.data() + static_cast<size_t>(cy0 + y) * width + cx0,
                        static_cast<size_t>(bw) * sizeof(float));
        }
        auto rec = recognizeCrop(api, I.recSession, I.memInfo, crop, bw, bh,
                                 I.recInputName, I.recOutputName, I.dict, &lastErr);
        if (rec.text.empty()) continue;

        OcrLine line;
        line.text = rec.text;
        line.confidence = rec.confidence;
        line.x = cx0;
        line.y = cy0;
        line.width = bw;
        line.height = bh;
        result.lines.push_back(std::move(line));
    }

    if (!lastErr.empty()) {
        LOG_WARN("OcrEngine: rec run error: {}", lastErr);
    }

    float sum = 0;
    for (const auto& l : result.lines) sum += l.confidence;
    result.meanConfidence = result.lines.empty() ? 0.0f : sum / result.lines.size();

    auto t1 = std::chrono::steady_clock::now();
    lastLatencyMs_ = std::chrono::duration<double, std::milli>(t1 - t0).count();
    totalLatencyMs_ += lastLatencyMs_;
    totalInferences_++;

    LOG_DEBUG("OcrEngine: recognized {} lines from {}x{} in {:.1f}ms",
              result.lines.size(), width, height, lastLatencyMs_);
    return result;
}

OcrPageResult OcrEngine::recognizePngBase64(const std::string& base64) {
    OcrPageResult result;
    std::vector<uint8_t> pixels;
    int w = 0, h = 0;
    if (!decodePngBase64(base64, pixels, w, h)) return result;
    return recognize(pixels.data(), w, h);
}

// ============================================================================
// Test hook: render text using the embedded bitmap font.
// ============================================================================

void OcrEngine::renderText(const std::string& text, int scale,
                           std::vector<uint8_t>& bgra, int& width, int& height) {
    int charW = 5 * scale + scale;
    int charH = 7 * scale;
    width = static_cast<int>(text.size()) * charW + 2 * scale;
    height = charH + 2 * scale;
    bgra.assign(static_cast<size_t>(width) * height * 4, 255);

    auto setInk = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        size_t idx = (static_cast<size_t>(y) * width + x) * 4;
        bgra[idx + 0] = 0; bgra[idx + 1] = 0; bgra[idx + 2] = 0; bgra[idx + 3] = 255;
    };

    for (size_t ci = 0; ci < text.size(); ++ci) {
        char ch = text[ci];
        if (ch < 32 || ch > 126) continue;
        const uint8_t* glyph = FONT5x7[ch - 32];
        int baseX = static_cast<int>(ci) * charW + scale;
        int baseY = scale;
        for (int gy = 0; gy < 7; ++gy) {
            for (int gx = 0; gx < 5; ++gx) {
                if ((glyph[gy] >> (4 - gx)) & 1) {
                    for (int sy = 0; sy < scale; ++sy) {
                        for (int sx = 0; sx < scale; ++sx) {
                            setInk(baseX + gx * scale + sx, baseY + gy * scale + sy);
                        }
                    }
                }
            }
        }
    }
}

} // namespace edgemon
