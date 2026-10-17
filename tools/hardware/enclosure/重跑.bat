@echo off
chcp 65001 >nul
set PY=python
echo === 建模 ===
%PY% "%~dp0build_case_v4.py"
if errorlevel 1 (echo 建模失败 & pause & exit /b 1)
echo === 可打印性校验 ===
%PY% "%~dp0check_printable.py"
pause