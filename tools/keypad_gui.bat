@echo off
rem Double-click launcher for the Nokia 3310 control panel.
cd /d "%~dp0"
where pyw >nul 2>nul && start "" pyw -3 "%~dp0keypad_gui.py" && exit /b
where pythonw >nul 2>nul && start "" pythonw "%~dp0keypad_gui.py" && exit /b
python "%~dp0keypad_gui.py"
