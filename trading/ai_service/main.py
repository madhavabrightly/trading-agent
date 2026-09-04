# trading/ai_service — local Python AI service (spec §4, §16, §15).
#
# C++ trading core  --HTTP-->  this service  --HTTP-->  LLM provider
#                                  |
#                                  +-- optional RAG (embeddings + vector store)
#
# Runs on 127.0.0.1 only. Provides:
#   GET  /health                 liveness
#   POST /analyze                structured analysis of OCR/news text
#   POST /rag/query              retrieve context passages (RAG)
#   POST /embed                  embeddings for RAG ingestion
#
# The LLM is reached through an OpenAI-compatible /chat/completions endpoint.
# Model/keys come from env:  AI_API_KEY, AI_BASE_URL, AI_MODEL, AI_PROVIDER.
#
# The AI only analyzes and recommends. It never places orders.

import json
import os
import urllib.error
import urllib.request
from typing import Any, Dict, List, Optional

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

app = FastAPI(title="trading-ai-service", version="0.1.0")

API_KEY: str = os.environ.get("AI_API_KEY", "")
BASE_URL: str = os.environ.get("AI_BASE_URL", "https://api.deepseek.com/v1").rstrip("/")
MODEL: str = os.environ.get("AI_MODEL", "deepseek-chat")
TIMEOUT: float = float(os.environ.get("AI_TIMEOUT", "30"))


# ---------------------------------------------------------------------------
# Schemas
# ---------------------------------------------------------------------------

class AnalyzeRequest(BaseModel):
    text: str
    role: str = "news"          # news | price | chart | alert | economic
    source: str = ""
    context: Optional[str] = None


class AnalyzeResponse(BaseModel):
    ok: bool = True
    type: str = "unknown"
    asset: str = ""
    sentiment: float = 0.0
    confidence: float = 0.0
    urgency: str = "low"
    summary: str = ""
    raw: str = ""
    error: str = ""


class RagQueryRequest(BaseModel):
    question: str
    top_k: int = 5


class RagQueryResponse(BaseModel):
    passages: List[str] = []


class EmbedRequest(BaseModel):
    texts: List[str]


class EmbedResponse(BaseModel):
    vectors: List[List[float]] = []


# ---------------------------------------------------------------------------
# Minimal in-memory RAG (optional). No vector DB required for a single-machine
# deployment; a real vector store can be swapped in behind this interface.
# ---------------------------------------------------------------------------

_docs: List[str] = []


def _embed(texts: List[str]) -> List[List[float]]:
    """Deterministic lexical hashing embeddings (no model dependency).
    Swap for sentence-transformers when installed."""
    try:
        import numpy as np  # type: ignore
    except ImportError:
        np = None

    out: List[List[float]] = []
    dim = 256
    for text in texts:
        vec = [0.0] * dim
        tokens = text.lower().split()
        if not tokens:
            out.append(vec)
            continue
        for tok in tokens:
            h = 2166136261
            for ch in tok.encode("utf-8"):
                h ^= ch
                h = (h * 16777619) & 0xFFFFFFFF
            idx = h % dim
            vec[idx] += 1.0
        norm = (sum(v * v for v in vec)) ** 0.5
        if norm > 0:
            vec = [v / norm for v in vec]
        out.append(vec)
    return out


def _cosine(a: List[float], b: List[float]) -> float:
    if not a or not b or len(a) != len(b):
        return 0.0
    return sum(x * y for x, y in zip(a, b))


# ---------------------------------------------------------------------------
# LLM call (OpenAI-compatible)
# ---------------------------------------------------------------------------

def _llm_chat(messages: List[Dict[str, str]], temperature: float = 0.2,
              max_tokens: int = 2048) -> str:
    if not API_KEY:
        raise RuntimeError("AI_API_KEY not set")
    payload = {
        "model": MODEL,
        "messages": messages,
        "temperature": temperature,
        "max_tokens": max_tokens,
        "stream": False,
    }
    req = urllib.request.Request(
        BASE_URL + "/chat/completions",
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": "Bearer " + API_KEY,
        },
    )
    with urllib.request.urlopen(req, timeout=TIMEOUT) as resp:
        body = json.loads(resp.read().decode("utf-8"))
    return body["choices"][0]["message"]["content"]


def _extract_json(text: str) -> Dict[str, Any]:
    """Pulls the first {...} block out of a model reply (handles fences)."""
    t = text.strip()
    if t.startswith("```"):
        lines = t.splitlines()
        if lines and lines[0].startswith("```"):
            lines = lines[1:]
        if lines and lines[-1].strip() == "```":
            lines = lines[:-1]
        t = "\n".join(lines)
    start = t.find("{")
    end = t.rfind("}")
    if start == -1 or end <= start:
        return {}
    try:
        return json.loads(t[start:end + 1])
    except json.JSONDecodeError:
        return {}


# ---------------------------------------------------------------------------
# Routes
# ---------------------------------------------------------------------------

@app.get("/health")
def health() -> Dict[str, Any]:
    return {"status": "ok", "model": MODEL, "rag_docs": len(_docs)}


@app.post("/analyze", response_model=AnalyzeResponse)
def analyze(req: AnalyzeRequest) -> AnalyzeResponse:
    prompt = (
        "You are a market-data analysis engine. Classify the following OCR "
        f"text captured from a trading page (role: {req.role}). "
        'Respond with STRICT JSON only: {"type":"news|economic|alert|price|'
        'chart|platform|unknown","asset":"CANONICAL_SYMBOL_OR_EMPTY",'
        '"sentiment":-1.0,"confidence":0.0,"urgency":"low|medium|high",'
        '"summary":"one short sentence"}. '
        "sentiment in [-1,1]; asset like BTC/USD or AAPL or empty. "
        "Never invent facts not in the text.\n"
        f"TEXT: {req.text}"
    )
    if not API_KEY:
        # Degraded mode: return a rule-based analysis so the pipeline can run
        # without an LLM (the C++ side already has classifier heuristics).
        resp = AnalyzeResponse()
        resp.ok = False
        resp.error = "AI_API_KEY not set; using heuristic fallback"
        return resp

    try:
        raw = _llm_chat([
            {"role": "system", "content": "You return strict JSON market analysis."},
            {"role": "user", "content": prompt},
        ])
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=502, detail=f"LLM error: {exc}")

    parsed = _extract_json(raw)
    if not parsed:
        raise HTTPException(status_code=502, detail="model returned no JSON")

    resp = AnalyzeResponse()
    resp.type = str(parsed.get("type", "unknown"))
    resp.asset = str(parsed.get("asset", ""))
    resp.sentiment = float(parsed.get("sentiment", 0.0))
    resp.confidence = float(parsed.get("confidence", 0.0))
    resp.urgency = str(parsed.get("urgency", "low"))
    resp.summary = str(parsed.get("summary", ""))
    resp.raw = raw
    resp.ok = True
    return resp


@app.post("/rag/ingest", status_code=201)
def rag_ingest(payload: Dict[str, Any]) -> Dict[str, Any]:
    """Adds documents to the in-memory RAG corpus. Body: {"documents":[str]}."""
    docs = payload.get("documents") or payload.get("texts") or []
    if not isinstance(docs, list):
        raise HTTPException(status_code=400, detail="documents must be a list")
    _docs.extend(str(d) for d in docs)
    return {"ingested": len(docs), "total": len(_docs)}


@app.post("/rag/query", response_model=RagQueryResponse)
def rag_query(req: RagQueryRequest) -> RagQueryResponse:
    if not _docs:
        return RagQueryResponse(passages=[])
    qv = _embed([req.question])[0]
    scored = sorted(
        ((_cosine(qv, _embed([d])[0]), d) for d in _docs),
        key=lambda x: x[0],
        reverse=True,
    )
    resp = RagQueryResponse()
    resp.passages = [d for score, d in scored[: req.top_k] if score > 0.01]
    return resp


@app.post("/embed", response_model=EmbedResponse)
def embed(req: EmbedRequest) -> EmbedResponse:
    return EmbedResponse(vectors=_embed(req.texts))


@app.post("/reset", status_code=200)
def reset() -> Dict[str, Any]:
    _docs.clear()
    return {"reset": True}
