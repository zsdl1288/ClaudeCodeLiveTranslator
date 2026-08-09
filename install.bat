@echo off
title Claude Code 中文版 - 安装
echo.
echo   ╔══════════════════════════════════════╗
echo   ║   Claude Code 中文化 安装程序       ║
echo   ╚══════════════════════════════════════╝
echo.

set "INSTALL_DIR=%USERPROFILE%\.claude\cli-zh"

echo [1/3] 安装 Python 依赖...
pip install pywinpty -q
if %ERRORLEVEL% NEQ 0 (
    echo 错误: pywinpty 安装失败
    pause
    exit /b 1
)

echo [2/3] 复制文件到 %INSTALL_DIR%...
if not exist "%INSTALL_DIR%" mkdir "%INSTALL_DIR%"
copy /Y "%~dp0claude_zh.py" "%INSTALL_DIR%\claude-zh.py" >nul
copy /Y "%~dp0translations.json" "%INSTALL_DIR%\translations.json" >nul

echo [3/3] 配置全局命令...
echo @echo off > "%APPDATA%\npm\claude-zh.cmd"
echo title Claude Code 中文版 >> "%APPDATA%\npm\claude-zh.cmd"
echo chcp 65001 ^>nul 2^>nul >> "%APPDATA%\npm\claude-zh.cmd"
echo python "%INSTALL_DIR%\claude-zh.py" %%* >> "%APPDATA%\npm\claude-zh.cmd"

:: 桌面快捷方式
powershell -Command "& {
    \$WshShell = New-Object -ComObject WScript.Shell;
    \$Desktop = [Environment]::GetFolderPath('Desktop');
    \$Shortcut = \$WshShell.CreateShortcut(\"\$Desktop\Claude Code 中文版.lnk\");
    \$Shortcut.TargetPath = 'wt.exe';
    \$Shortcut.Arguments = '--title \"Claude Code 中文版\" python \"' + \$env:USERPROFILE + '\.claude\cli-zh\claude-zh.py\"';
    \$Shortcut.IconLocation = \$env:APPDATA + '\npm\node_modules\@anthropic-ai\claude-code\bin\claude.exe,0';
    \$Shortcut.Save();
}" >nul 2>nul

echo.
echo   ✓ 安装完成!
echo.
echo   现在你可以在终端输入 claude-zh 启动
echo   或者双击桌面的 "Claude Code 中文版" 图标
echo.
pause
