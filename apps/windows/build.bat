@echo off
rem Builds dist\PipLink.exe with PyInstaller.
chcp 65001 >nul
cd /d "%~dp0"
python -m pip install --disable-pip-version-check -r requirements.txt pyinstaller
if errorlevel 1 exit /b 1
pyinstaller --onedir --console --name PipLink --distpath dist --workpath build --specpath build pip_link.py
echo.
echo Готово: dist\PipLink\PipLink.exe
pause
