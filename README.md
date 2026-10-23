# Claude 桌面实时翻译 · C++ 版 v1.0.0

Windows 10/11 x64 的原生 C++ 独立翻译窗口，继续配合现有 Claude 桌面客户端使用。程序通过 Windows UI Automation 只读获取客户端公开的助手文字，按稳定句子增量翻译。

不使用 OCR、截图、客户端补丁、注入、内部网络拦截或自动输入。不会改动 Claude 的安装文件、配置、登录状态或对话。

## 使用

从 [v1.0.0 Release](https://github.com/zsdl1288/ClaudeCodeLiveTranslator/releases/tag/v1.0.0) 下载 `ClaudeTranslator-v1.0.0-windows-x64.zip`，完整解压后双击 `ClaudeTranslator.exe`。这是 Windows 10/11 x64 便携版，无需安装；请解压到有写入权限的文件夹，以便保存设置。

源码用户编译后可双击项目根目录的 `启动翻译.cmd`，或运行 `dist\ClaudeTranslator\ClaudeTranslator.exe`。离线演示可运行 `ClaudeTranslator.exe --demo`；源码目录也提供 `离线演示.cmd`。

1. 正常打开 Claude 桌面客户端，进入会话。
2. 在翻译窗口刷新窗口列表，选择 Claude 窗口，然后开始读取。
3. 继续在 Claude 中工作；译文在独立窗口显示，标题注明当前任务。切换 Claude 任务或窗口后会自动跟随。
4. 暂停、停止只作用于本工具，Claude 不会被关闭或重启。

默认只显示中文译文，正文为 10pt，使用紧凑布局，不添加软件序号。原文与译文使用相同的原生富文本排版，支持标题、粗体、列表与段落样式；阅读区域用细灰线分隔，不显示原文和译文栏标题。能识别的 Markdown 标记会转换为排版，不执行内容。点击「回到最新」会立即滚到底部并自动跟随后续译文；手动向上阅读时暂停跟随，再次点击即可恢复。界面采用暖灰深色、柔和铜色重点与圆角按钮，应用图标嵌入 EXE。可设置显示原文、窗口置顶和正文字号（8–40pt）；保存字号后立即重排现有译文，标题、列表和代码样式随字号一起调整。工具栏仅保留设置、清空、重试失败和回到最新，不提供导出或复制译文按钮。双击 `离线演示.cmd` 可用合成句检查窗口和翻译流程，演示不读取 Claude、不联网。

运行时不需要 Python、.NET 或额外安装 C++ 运行库。采集器是同一个原生 EXE 的独立低优先级子进程。建议移动或分发完整的 `dist/ClaudeTranslator` 文件夹，以保留说明及开源许可证。

## 命令行启动与状态诊断

自动选择当前列表中的第一个 Claude 窗口并开始读取（默认不开启）：

```bat
ClaudeTranslator.exe --auto-start
```

显式开启小型状态文件，用于检查是否已连接、读到正文和完成翻译：

```bat
dist\ClaudeTranslator\ClaudeTranslator.exe --auto-start --status-file "%CD%\diagnostic-status.json"
```

`--status-file` 必须使用绝对路径。程序每秒最多通过一个后台写入任务原子更新一次 JSON；默认不创建状态文件。文件只含采集序号、读取耗时、块数和字数、运行/暂停状态、记录/成功/错误数量、译文字数、所选窗口是否最小化及来源代次，不保存原文、译文、标题、会话标识、任务 key 或密钥。`capture_seq` 持续增长且 `captured_chars` 为 0 表示采集器有响应，但当前没有识别到正文；`records` 大于 0 而 `done` 暂未增加时，文字可能正在稳定或等待翻译服务。

可用下面的纯离线自检验证自动启动参数及状态文件，不读取 Claude、不联网：

```bat
build-cpp\ClaudeTranslator.exe --self-test --auto-start --status-file "%CD%\build-cpp\diagnostic-check.json" --report "%CD%\build-cpp\gui-diagnostic-check.json"
```

## 性能与延迟

- 后台采集不抢焦点，不点击、不滚动、不发送按键，也不写入剪贴板。
- 辅助功能查询在独立进程进行，翻译在后台线程进行，界面不等待网络完成。
- 查询串行执行；读取变慢时降低轮询频率。接口无响应时，只重启本工具的采集器。
- 默认只处理最新可识别的助手消息；按已稳定句子增量排队，避免积压历史消息。
- 默认稳定等待为 900 毫秒，可在设置中调整。采集耗时和翻译服务响应还会增加延迟；不能保证绝对零开销或零卡顿。

## 本机 Code 页实测（2026-10-22）

测试客户端为 Windows Claude 2.42304.2.0，机器有 24 个逻辑处理器。使用独立、无文件夹的 Code 测试会话，仅生成通用英文，不让 Claude 调用工具或读写项目。

- 普通英文生成期间，读取字数从 87 增长到 5,305，多次触发增量翻译；Claude 在后台、但没有最小化时仍能读取。恢复后两轮共 100 条测试句全部完成翻译，无重复条目或翻译错误。
- 最小化阶段，正文短暂增加后停在 5,890 字符；恢复后读取到完整的 8,161 字符并补齐译文。因此当前不能保证 Claude 最小化后的持续更新。建议保持目标会话打开，将 Claude 放在后台，使用置顶翻译窗口；不要依赖最小化来保持实时翻译。
- 45 秒资源采样中，翻译窗口与读取器合计平均占整机 CPU 约 0.79%，同帧峰值约 2.74%，合计工作集约 53–71 MiB。流式增长阶段平均约 1.18% CPU；读取耗时 149–396 毫秒。没有观察到无响应或进程重启。短时测试不能保证所有机器和客户端版本都绝对无卡顿。
- 上述是通用英文和当前已加载内容的测试结果，不代表折叠、虚拟化或客户端未向辅助功能公开的全部内容都能读取。
## 翻译服务与数据去向

默认使用 Google 无 Key 网页翻译接口。选择它时，待译的助手文字会通过 HTTPS 发送给 Google。该接口不是稳定的官方 API，可能限流、不可用或改变行为。

设置还支持：

- 兼容 Chat Completions 的服务：填写服务地址、模型名称和 API Key 环境变量名。待译文字会发送给该地址。
- 本地 Ollama：通常填写 `http://127.0.0.1:11434` 和本机已安装的模型名；使用本地服务时，待译文字可留在本机。

API Key 值不写入配置文件，也不传给只读采集器。程序不会在失败时自动切换到别的服务。云端翻译的网络可用性和费用取决于所选服务。

## 能读取的内容

只能读取 Claude 向 Windows 辅助功能公开、已加载且能识别为助手输出的文字，包括部分公开思考和中间输出。代码块不翻译。

Claude 普通 Chat 和 Code 页面暴露的辅助功能结构可能不同，也可能随客户端版本改变。折叠、未加载、虚拟化或没有公开文字语义的内容可能读取不到；这时窗口显示状态，不会使用截图或修改客户端来绕过限制。

切换任务时取消已失效的翻译请求，分别保留各任务的译文和阅读位置；切回来后恢复缓存，对已完成且原文相同的段落复用译文，只翻译新内容。优先用客户端辅助功能公开的对话 URL 区分任务，改名不影响缓存；未公开稳定标识时使用降级身份，无法保证所有匿名页面或客户端重启后的任务识别。

## 内存缓存与本地设置

- 设置保存到 EXE 所在目录的 `data/config.json`。
- 各任务的标题、原文、译文与阅读位置仅缓存在当前进程内存中，不生成本地缓存文件；关闭软件后自动释放。
- 每个任务仅保留最新 100 个逻辑字幕行，超过后从最旧内容开始裁剪。逻辑行以文本换行区分，窗口宽度导致的自动折行不另外计数；缓存本身也会裁剪，并非只隐藏显示内容。
- 「清空」只清除当前任务的内存内容，其他任务缓存保留；重启软件不会恢复上次内容。
- API Key 不加入任务缓存；程序不提供译文导出文件功能。
- 不读取 Claude 的登录令牌、Cookie 或账号文件，不写入 Claude 的安装或账号目录。
## 编译与测试

源码全部采用 C++17，使用 Win32、Windows UI Automation、WinHTTP 和 nlohmann/json。仓库包含 JSON 头文件，编译器需另行下载。

使用便携工具链构建：

1. 从 [LLVM-MinGW 20260908 官方发布页](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260908) 下载 `llvm-mingw-20260908-ucrt-x86_64.zip`，按 `third_party/DEPENDENCIES.txt` 中的 SHA256 校验。
2. 将完整工具链解压为 `toolchain/llvm-mingw-20260908-ucrt-x86_64/`，确认其中有 `bin/x86_64-w64-mingw32-clang++.exe` 和 `bin/llvm-windres.exe`。无需修改系统 PATH。
3. 在 Windows 命令提示符中执行下方构建命令。工具链、构建产物和本机配置不会提交到仓库。

在项目目录执行：

```bat
build.cmd --tests --stage
```

此命令将原生 EXE 和测试程序写入 `build-cpp/`，并运行核心回归检查、合成辅助功能窗口测试和隐藏窗口的原生 GUI 离线自检。GUI 自检报告位于 `build-cpp/gui-check.json` 及 `build-cpp/gui-diagnostic-check.json`，验证离线译文完成且没有读取 Claude 或调用外网。测试不会读取真实 Claude 消息。翻译服务本地回归测试只使用本机回环接口。

```bat
build.cmd --tests
```

此命令在测试通过后，将成品及许可证复制到 `dist/ClaudeTranslator/`。更新前请关闭正在运行的翻译窗口。也可通过 `CCLIVE_CXX` 指定其他兼容 clang++/g++ 编译器，通过 `CCLIVE_WINDRES` 指定兼容 LLVM/GNU 的资源编译器。

`CMakeLists.txt` 提供标准 CMake 构建入口，支持 Windows MSVC 或 MinGW 工具链；项目自带的 `build.cmd` 不依赖 CMake。工具链与 JSON 头文件的固定版本、官方来源和已验证 SHA256 见 `third_party/DEPENDENCIES.txt`。

主要文件：

- `src/main.cpp`：原生窗口、后台任务、独立采集进程监督。
- `src/reader.cpp`：只读辅助功能助手文字提取。
- `src/core.cpp`：设置、稳定分句、去重和翻译服务。
- `src/task_cache.cpp`、`include/task_cache.hpp`：按任务区分的纯内存缓存与容量限制。
- `src/text_layout.cpp`、`include/text_layout.hpp`：标题、粗体、列表及段落的原生富文本排版。
- `assets/app-icon.svg`、`assets/app.ico`、`resources/app.rc`：应用矢量图标、多尺寸 Windows 图标与嵌入资源。
- `include/`：公共 C++ 接口。
- `tests/core_tests.cpp`：分句、版本、配置及翻译协议回归。
- `tests/fixture.cpp`、`tests/reader_smoke.cpp`：合成助手输出增量读取测试。

## 开源来源

最初基础项目为 MIT 的 [claude-code-zh](https://github.com/liuxiao20051106-prog/claude-code-zh)，根目录保留其 MIT 许可。当前源码已重写为原生 C++。

辅助功能提取逻辑参考 MIT 的 [claude-desktop-translator](https://github.com/yxue301603/claude-desktop-translator)，上游提交 `a8eaeed556eea71e0f58eb7364043b0be47b5116`，许可证见 `third_party/desktop-translator-LICENSE`。

JSON 库为 MIT 的 [nlohmann/json](https://github.com/nlohmann/json) v3.12.0。静态链接的 LLVM C++ 运行时许可证见 `third_party/llvm-LICENSE`，MinGW 运行时声明保存在 `third_party/mingw/`。成品的 `licenses/` 中包含相应声明。

这是独立社区项目，与 Anthropic 没有隶属关系。
