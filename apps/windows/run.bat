@echo off
rem Pip Link for Windows: install what is missing and start.
chcp 65001 >nul
cd /d "%~dp0"
python -m pip install --disable-pip-version-check -r requirements.txt
if errorlevel 1 (
  echo.
  echo Не удалось установить зависимости. Проверь, что Python установлен и есть интернет.
  pause
  exit /b 1
)
python pip_link.py
pause
