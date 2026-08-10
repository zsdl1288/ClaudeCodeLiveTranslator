# Claude Code 中文化 (claude-code-zh)

让 Claude Code 的终端界面自动显示中文 — 无需手动维护翻译词条，内置 Google 翻译引擎 + 智能缓存。

## ✨ 特性

- **自动翻译** — 使用 Google Translate 引擎实时翻译界面英文为中文
- **智能缓存** — 翻译结果缓存到本地，越用越快
- **中文输入** — 完美支持中文 IME 输入
- **双击启动** — 桌面快捷方式一键启动

## 效果

```
原来的英文界面                      中文化后
──────────────────────────────    ──────────────────────────────
Accessing workspace:               正在访问工作区:
Quick safety check: Is this...    安全检查: 这是你创建或信任...
Yes, I trust this folder           是，我信任此文件夹
Do you want to allow...            您想允许该工具调用吗？
Thinking…                          思考中…
Allow / Deny                       允许 / 拒绝
```

## 安装

### 前提条件

- Windows 10/11
- Python 3.7+
- [Claude Code](https://docs.anthropic.com/en/docs/claude-code) 已安装

### 一键安装

双击 `install.bat`，或以管理员身份运行：

```cmd
install.bat
```

### 手动安装

```cmd
pip install pywinpty deep-translator
git clone https://github.com/YOUR_USERNAME/claude-code-zh.git
```

然后将 `claude-code-zh` 目录添加到系统 PATH。

## 使用

安装后，直接在终端输入：

```cmd
claude-zh
```

或者在桌面上找到 **「Claude Code 中文版」** 快捷方式，双击启动。

## 原理

通过 Windows ConPTY 伪终端包装 Claude Code，使用 Google Translate 实时翻译输出中的英文为中文。翻译结果自动缓存到 `trans_cache.json`，下次启动直接使用，无需重复翻译。

`translations.json` 中的词条作为初始缓存种子，确保常用文本在首次启动时即显示中文。

## 自定义

编辑 `translations.json` 可添加或覆盖翻译（可选，不编辑也能自动翻译）：

```json
{
  "phrases": [
    ["Some English UI text", "对应的中文翻译"]
  ]
}
```

## 许可

MIT License
