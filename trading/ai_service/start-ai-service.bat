# Starts the local Python AI service on 127.0.0.1:8765.
#   start-ai-service.bat
#
# Optional env (set before running):
#   AI_API_KEY   your LLM API key (or leave unset for heuristic fallback)
#   AI_BASE_URL  OpenAI-compatible base URL
#   AI_MODEL     model name

@echo off
setlocal
cd /d "%~dp0"
echo Starting trading AI service on http://127.0.0.1:8765 ...
python -m uvicorn main:app --host 127.0.0.1 --port 8765 %*
endlocal
