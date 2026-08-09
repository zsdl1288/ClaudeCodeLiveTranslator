# Claude Code 中文化 (claude-code-zh)

让 Claude Code 的终端界面显示中文 — 权限提示、状态栏、按钮文字统统变成中文。

## 效果

```
原来的英文界面                      中文化后
──────────────────────────────    ──────────────────────────────
Accessing workspace:               正在访问工作区:
Quick safety check: Is this...    安全检查: 这是你创建或信任...
Yes, I trust this folder           是，我信任此文件夹
No, exit                           否，退出
manual mode on                     手动模式
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
pip install pywinpty
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

通过 Windows ConPTY 伪终端包装 Claude Code，在不影响交互体验的前提下实时拦截输出文字并替换为中文翻译。

## 自定义翻译

编辑同目录下的 `translations.json`，在 `phrases` 数组中添加你自己的翻译：

```json
{
  "phrases": [
    ["Some English UI text", "对应的中文翻译"]
  ]
}
```

## 许可

MIT License
