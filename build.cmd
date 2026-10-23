@echo off
setlocal
cd /d "%~dp0"
set "CXX=%~dp0toolchain\llvm-mingw-20260908-ucrt-x86_64\bin\x86_64-w64-mingw32-clang++.exe"
if defined CCLIVE_CXX set "CXX=%CCLIVE_CXX%"
if not exist "%CXX%" (
  echo C++ compiler missing. Set CCLIVE_CXX to a clang++ or g++ executable.
  exit /b 2
)
set "WINDRES=%~dp0toolchain\llvm-mingw-20260908-ucrt-x86_64\bin\llvm-windres.exe"
if defined CCLIVE_WINDRES set "WINDRES=%CCLIVE_WINDRES%"
if not exist "%WINDRES%" (
  echo Resource compiler missing. Set CCLIVE_WINDRES to an LLVM or GNU windres executable.
  exit /b 2
)
set "RUN_TESTS=0"
set "STAGE_ONLY=0"
for %%A in (%*) do (
  if /i "%%~A"=="--tests" set "RUN_TESTS=1"
  if /i "%%~A"=="--stage" set "STAGE_ONLY=1"
)
if not exist "build-cpp" mkdir "build-cpp"
set "FLAGS=-std=c++17 -O2 -static -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -Iinclude -Ithird_party -Wall -Wextra -Wno-unknown-pragmas"
set "LIBS=-luiautomationcore -lole32 -loleaut32 -luuid -lwinhttp -lcomctl32 -lcomdlg32 -lshell32 -lshlwapi -lgdi32 -luser32 -ladvapi32 -lws2_32 -lcrypt32"
echo Building embedded application icon...
"%WINDRES%" -Iresources -Iinclude -O coff -i resources\app.rc -o build-cpp\app-resource.o
if errorlevel 1 exit /b 1
echo Building native C++ desktop application...
"%CXX%" %FLAGS% -municode -mwindows src\main.cpp src\reader.cpp src\core.cpp src\chatgpt.cpp src\codex_cli.cpp src\task_cache.cpp src\text_layout.cpp build-cpp\app-resource.o -o build-cpp\ClaudeTranslator.exe %LIBS%
if errorlevel 1 exit /b 1
if "%RUN_TESTS%"=="0" goto package
echo Building core regression tests...
"%CXX%" %FLAGS% tests\core_tests.cpp src\core.cpp src\chatgpt.cpp src\codex_cli.cpp -o build-cpp\core_tests.exe %LIBS%
if errorlevel 1 exit /b 1
echo Building ChatGPT account and protocol regression tests...
"%CXX%" %FLAGS% -DCCLIVE_CHATGPT_TESTING tests\chatgpt_tests.cpp src\chatgpt.cpp src\codex_cli.cpp src\core.cpp -o build-cpp\chatgpt_tests.exe %LIBS%
if errorlevel 1 exit /b 1
echo Building Codex CLI protocol and process regression tests...
"%CXX%" %FLAGS% -DCCLIVE_CODEX_CLI_TESTING -municode tests\codex_cli_tests.cpp src\codex_cli.cpp src\core.cpp src\chatgpt.cpp -o build-cpp\codex_cli_tests.exe %LIBS%
if errorlevel 1 exit /b 1
echo Building task cache regression tests...
"%CXX%" %FLAGS% tests\task_cache_tests.cpp src\task_cache.cpp -o build-cpp\task_cache_tests.exe %LIBS%
if errorlevel 1 exit /b 1
echo Building text layout regression tests...
"%CXX%" %FLAGS% tests\text_layout_tests.cpp src\text_layout.cpp -o build-cpp\text_layout_tests.exe %LIBS%
if errorlevel 1 exit /b 1
if not exist tests\fixture.cpp goto run_tests
echo Building read-only accessibility fixture...
"%CXX%" %FLAGS% -municode -mwindows tests\fixture.cpp -o build-cpp\CCLiveFixture.exe %LIBS%
if errorlevel 1 exit /b 1
if not exist tests\reader_smoke.cpp goto run_tests
echo Building accessibility smoke test...
"%CXX%" %FLAGS% -municode tests\reader_smoke.cpp src\core.cpp src\chatgpt.cpp src\codex_cli.cpp -o build-cpp\reader_smoke.exe %LIBS%
if errorlevel 1 exit /b 1
:run_tests
build-cpp\core_tests.exe
if errorlevel 1 exit /b 1
build-cpp\chatgpt_tests.exe
if errorlevel 1 exit /b 1
build-cpp\codex_cli_tests.exe
if errorlevel 1 exit /b 1
build-cpp\task_cache_tests.exe
if errorlevel 1 exit /b 1
build-cpp\text_layout_tests.exe
if errorlevel 1 exit /b 1
if not exist build-cpp\reader_smoke.exe goto gui_test
build-cpp\reader_smoke.exe build-cpp\CCLiveFixture.exe build-cpp\ClaudeTranslator.exe
if errorlevel 1 exit /b 1
:gui_test
echo Running offline native GUI self-test...
start "" /wait "%~dp0build-cpp\ClaudeTranslator.exe" --self-test --report "%~dp0build-cpp\gui-check.json"
if errorlevel 1 exit /b 1
echo Running explicitly enabled offline metadata diagnostic check...
start "" /wait "%~dp0build-cpp\ClaudeTranslator.exe" --self-test --auto-start --status-file "%~dp0build-cpp\diagnostic-check.json" --report "%~dp0build-cpp\gui-diagnostic-check.json"
if errorlevel 1 exit /b 1
:package
if "%STAGE_ONLY%"=="1" goto done
if not exist "dist\ClaudeTranslator" mkdir "dist\ClaudeTranslator"
copy /y build-cpp\ClaudeTranslator.exe dist\ClaudeTranslator\ClaudeTranslator.exe >nul
if errorlevel 1 (
  echo Cannot replace the running executable. Close Claude Translator and run build.cmd again.
  exit /b 1
)
copy /y LICENSE dist\ClaudeTranslator\LICENSE >nul
if exist README.md copy /y README.md dist\ClaudeTranslator\README.md >nul
if exist CHANGELOG.md copy /y CHANGELOG.md dist\ClaudeTranslator\CHANGELOG.md >nul
if not exist "dist\ClaudeTranslator\licenses" mkdir "dist\ClaudeTranslator\licenses"
copy /y third_party\nlohmann\LICENSE dist\ClaudeTranslator\licenses\nlohmann-json-LICENSE >nul
if exist third_party\llvm-LICENSE copy /y third_party\llvm-LICENSE dist\ClaudeTranslator\licenses\llvm-LICENSE >nul
if exist third_party\desktop-translator-LICENSE copy /y third_party\desktop-translator-LICENSE dist\ClaudeTranslator\licenses\desktop-translator-LICENSE >nul
if not exist "dist\ClaudeTranslator\licenses\mingw" mkdir "dist\ClaudeTranslator\licenses\mingw"
copy /y third_party\mingw\* dist\ClaudeTranslator\licenses\mingw\ >nul
:done
echo Native C++ build complete.
exit /b 0

