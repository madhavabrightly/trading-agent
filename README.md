# Trading Agent

A C++20 trading agent that combines a browser-based "edge monitor" (Edge via
CDP) with OCR, an AI analysis layer, and a broker connector for algorithmic
trading signals.

> **Status: development / experimental.** Paper trading by default. Not
> financial advice.

## What's here

```
src/        Edge monitor core: Edge discovery, CDP client, page capture,
            DOM/OCR extraction, embeddings, SQLite storage
include/    Public headers for the edge monitor libraries
trading/    Trading core: connectors (Alpaca), market data, AI analysis,
            signals, strategies, risk engine, order execution, credential store
tests/      Unit + integration tests (CTest)
third_party Vendored build deps (ONNX Runtime, SQLite) — not tracked; see below
```

## Features

- **Edge monitor** — discovers running Microsoft Edge instances and attaches
  via the Chrome DevTools Protocol, captures pages, and extracts content
  through DOM observation and OCR.
- **OCR pipeline** — PP-OCRv3 text recognition (ONNX Runtime), text
  normalization, and change detection on captured frames.
- **AI layer** — structured analysis of OCR/news/alert text through an
  OpenAI-compatible LLM provider, with an optional local Python (FastAPI)
  service for RAG.
- **Trading core** — broker abstraction with an Alpaca connector (paper by
  default, live only when explicitly configured), market data streaming,
  signal fusion, risk engine, and a paper simulator for backtesting
  strategies without real orders.
- **Secret handling** — API keys/secrets are stored in Windows Credential
  Manager or a DPAPI-encrypted local file; they are never committed and never
  logged.

## Building

Requirements: CMake ≥ 3.20, a C++20 compiler (MSVC on Windows), OpenSSL.

```bat
cmake -S . -B build
cmake --build build --config Release
```

Dependencies are pulled at configure time (nlohmann/json via FetchContent).
For OCR you additionally need the PP-OCRv3 ONNX models in `models/`, and ONNX
Runtime in `third_party/onnxruntime/` (both ignored by git — download them
locally).

## Config & credentials

Broker credentials are resolved through `trading::CredentialStore`: Windows
Credential Manager → DPAPI-encrypted file → environment variables. No secrets
belong in source or config files.

## License

[Apache-2.0](LICENSE)
