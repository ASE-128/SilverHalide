@echo off
REM Aider launcher for SilverHalide project
REM Sets required environment variables for Ollama + UTF-8 on Windows

REM Ollama API endpoint (aider uses this env var, NOT openai-api-base config)
set OLLAMA_API_BASE=http://localhost:11434

REM Force Python UTF-8 mode to prevent GBK codec errors on Windows
set PYTHONUTF8=1

REM Optional: suppress SSL verification if behind corporate proxy
REM set AIDER_VERIFY_SSL=false

REM Optional: skip model price list download (avoids GitHub SSL errors)
REM set AIDER_CHECK_UPDATE=false

echo [aider.bat] OLLAMA_API_BASE=%OLLAMA_API_BASE%
echo [aider.bat] PYTHONUTF8=%PYTHONUTF8%
echo.

REM Launch aider with all arguments passed through
"C:\Users\tmlde\.local\bin\aider.exe" %*
