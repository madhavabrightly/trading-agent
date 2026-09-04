#pragma once

// Local real OCR engine: PP-OCRv3 (detection + recognition) run through the
// ONNX Runtime C API, loaded dynamically at runtime (no link-time dependency,
// MinGW-safe). Pipeline:
//   PNG -> GDI+ decode -> BGRA -> grayscale -> det model (text boxes)
//   -> per-box crop -> resize -> rec model -> CTC decode -> text + confidence.
//
// OcrEngine is the stable interface consumed by OCRManager / the watcher.
// It reports engine availability separately from recognition results so callers
// never mistake "no engine" for "no text".

#include "core/Types.hpp"
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

namespace edgemon {

struct OcrLine {
    std::string text;
    float confidence = 0.0f;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

struct OcrPageResult {
    std::vector<OcrLine> lines;
    float meanConfidence = 0.0f;

    std::string fullText() const;
    bool empty() const { return lines.empty(); }
};

class OcrEngine {
public:
    OcrEngine();
    ~OcrEngine();

    OcrEngine(const OcrEngine&) = delete;
    OcrEngine& operator=(const OcrEngine&) = delete;

    // Loads the ONNX models (det + rec + dict). Call once before recognize().
    // Safe to call multiple times (idempotent). Returns true if the real OCR
    // engine is ready; check lastError() on failure.
    bool loadModels(const std::string& modelsDir = "models");

    // True when both ONNX models loaded and the runtime is usable.
    bool available() const;

    // Human-readable reason when available() == false.
    const std::string& lastError() const { return error_; }

    // Decode PNG bytes to BGRA pixels via GDI+.
    static bool decodePng(const std::vector<uint8_t>& pngBytes,
                          std::vector<uint8_t>& pixels,
                          int& width, int& height);

    // Decode base64-encoded PNG bytes to BGRA pixels via GDI+.
    static bool decodePngBase64(const std::string& base64,
                                std::vector<uint8_t>& pixels,
                                int& width, int& height);

    // Recognize text from raw BGRA pixels using the real OCR models.
    // Returns an empty result (lines.empty()) when the engine is unavailable
    // or nothing was detected — never fabricates text.
    OcrPageResult recognize(const uint8_t* bgra, int width, int height);

    // Recognize from a base64 PNG (convenience).
    OcrPageResult recognizePngBase64(const std::string& base64);

    // Wall-clock time of the last recognize() call (det+rec inference only).
    double lastLatencyMs() const { return lastLatencyMs_; }

    // Cumulative stats since load.
    size_t totalInferences() const { return totalInferences_; }
    double totalLatencyMs() const { return totalLatencyMs_; }

    // Test hook: render a known text string to a bitmap using a bitmap font so
    // tests can verify the recognizer round-trips real text.
    static void renderText(const std::string& text, int scale,
                           std::vector<uint8_t>& bgra, int& width, int& height);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::string error_;
    double lastLatencyMs_ = 0.0;
    size_t totalInferences_ = 0;
    double totalLatencyMs_ = 0.0;
};

} // namespace edgemon
