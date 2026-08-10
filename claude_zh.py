#!/usr/bin/env python3
"""
Claude Code CLI 中文化包装器 v3.0 — 自动翻译版
通过 PTY 拦截 Claude Code 输出，自动翻译英文为中文。
不需要手动维护翻译词条 — 内置翻译引擎 + 智能缓存。

用法:  python claude-zh.py [参数...]
"""

import sys
import os
import json
import re
import time
import threading

# ═══════════════════════════════════════════════════════════════
# UTF-8 输出
# ═══════════════════════════════════════════════════════════════
if sys.platform == "win32":
    for s in (sys.stdout, sys.stderr):
        try:
            s.reconfigure(encoding="utf-8", errors="replace")
        except Exception:
            pass

# ═══════════════════════════════════════════════════════════════
# 自动翻译引擎 v3.0
# ═══════════════════════════════════════════════════════════════
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# ── 翻译缓存 ──
_trans_cache = {}

# 尝试加载翻译库 (pip install deep-translator)
_translator = None
try:
    from deep_translator import GoogleTranslator as _Translator
    _translator = _Translator(source='en', target='zh-CN')
except ImportError:
    pass

# 从旧词条表导入已验证翻译作为缓存种子
_TRANS_PATH = os.path.join(_SCRIPT_DIR, "translations.json")
if os.path.exists(_TRANS_PATH):
    try:
        with open(_TRANS_PATH, "r", encoding="utf-8") as f:
            data = json.load(f)
        for eng, chn in data.get("phrases", []):
            _trans_cache[eng.strip()] = chn
    except Exception:
        pass

# 加载持久化缓存
_CACHE_PATH = os.path.join(_SCRIPT_DIR, "trans_cache.json")
if os.path.exists(_CACHE_PATH):
    try:
        with open(_CACHE_PATH, "r", encoding="utf-8") as f:
            _trans_cache.update(json.load(f))
    except Exception:
        pass

def _save_cache():
    try:
        with open(_CACHE_PATH, "w", encoding="utf-8") as f:
            json.dump(_trans_cache, f, ensure_ascii=False, indent=2)
    except Exception:
        pass

# ── 跳过翻译的文本模式 ──
_SKIP_PATTERNS = [
    re.compile(r'^[A-Za-z]:[\\/]'),           # C:\...
    re.compile(r'^[\\/]{1,2}'),                # / 或 \\
    re.compile(r'^\.{1,2}[\\/]'),              # ./  ../
    re.compile(r'^(https?|git|ssh|ws)://'),    # URLs
    re.compile(r'^[0-9.,;:!?@#$%^&*()\[\]{}<>=+|/\\_-]+$'),  # 纯符号/数字
    re.compile(r'^\s*$'),                      # 空白
]

def _should_translate(text: str) -> bool:
    if not text or len(text.strip()) < 3:
        return False
    ascii_count = sum(1 for c in text if ord(c) < 128)
    total = len(text.strip())
    if ascii_count < total * 0.35:
        return False
    stripped = text.strip()
    for pat in _SKIP_PATTERNS:
        if pat.match(stripped):
            return False
    letters = sum(1 for c in text if c.isascii() and c.isalpha())
    return letters >= 3

def _auto_translate(text: str) -> str:
    key = text.strip()
    if not key:
        return text
    leading = text[:len(text) - len(text.lstrip())]
    trailing = text[len(text.rstrip()):]
    if key in _trans_cache:
        return leading + _trans_cache[key] + trailing
    if _translator is not None:
        try:
            result = _translator.translate(key)
            if result and result != key:
                _trans_cache[key] = result
                return leading + result + trailing
        except Exception:
            pass
    return text

def _translate(text: str) -> str:
    if not text:
        return ""
    if '\x1b' not in text:
        return _auto_translate(text) if _should_translate(text) else text
    parts = re.split(r'(\x1b\[[0-?]*[ -/]*[@-~])', text)
    result = []
    for part in parts:
        if part.startswith('\x1b'):
            result.append(part)
        elif _should_translate(part):
            result.append(_auto_translate(part))
        else:
            result.append(part)
    return ''.join(result)

# ═══════════════════════════════════════════════════════════════
# 查找 claude.exe
# ═══════════════════════════════════════════════════════════════
def _find_claude_exe():
    known = [
        os.path.expandvars(r"%APPDATA%\npm\node_modules\@anthropic-ai\claude-code\bin\claude.exe"),
        os.path.expandvars(r"%LOCALAPPDATA%\claude\claude.exe"),
    ]
    for p in known:
        if os.path.isfile(p):
            return p
    for d in os.environ.get("PATH", "").split(os.pathsep):
        p = os.path.join(d, "claude.exe")
        if os.path.isfile(p):
            return p
    return None

# ═══════════════════════════════════════════════════════════════
# Windows 终端辅助
# ═══════════════════════════════════════════════════════════════
if sys.platform == "win32":
    import ctypes
    _kernel32 = ctypes.windll.kernel32
    _STD_OUTPUT_HANDLE = -11
    _h_out = _kernel32.GetStdHandle(_STD_OUTPUT_HANDLE)

    _ENABLE_VT = 0x0004
    _out_mode = ctypes.c_uint32()
    _kernel32.GetConsoleMode(_h_out, ctypes.byref(_out_mode))
    _kernel32.SetConsoleMode(_h_out, _out_mode.value | _ENABLE_VT)

    def _get_win_size():
        buf = ctypes.create_string_buffer(22)
        _kernel32.GetConsoleScreenBufferInfo(_h_out, buf)
        top    = int.from_bytes(buf[12:14], 'little')
        bottom = int.from_bytes(buf[16:18], 'little')
        left   = int.from_bytes(buf[10:12], 'little')
        right  = int.from_bytes(buf[14:16], 'little')
        rows = bottom - top + 1
        cols = right - left + 1
        return max(rows, 1), max(cols, 1)
else:
    def _get_win_size():
        try:
            import fcntl, struct, termios
            buf = fcntl.ioctl(sys.stdout.fileno(), termios.TIOCGWINSZ, b'\x00' * 4)
            rows, cols = struct.unpack('HH', buf)
            return max(rows, 1), max(cols, 1)
        except Exception:
            return 30, 120

# ═══════════════════════════════════════════════════════════════
# 主程序
# ═══════════════════════════════════════════════════════════════
def main():
    from winpty import PtyProcess

    claude_exe = _find_claude_exe()
    if not claude_exe:
        print("错误: 找不到 claude.exe", file=sys.stderr)
        sys.exit(1)

    args = [claude_exe] + sys.argv[1:]
    env = os.environ.copy()
    env["CLAUDE_CLI_ZH_WRAPPED"] = "1"

    print("\033[1;36mClaude Code 中文化 v3.0 (自动翻译)\033[0m")
    cache_count = len(_trans_cache)
    if cache_count:
        print(f"\033[90m已加载 {cache_count} 条翻译缓存\033[0m")
    if _translator is None:
        print("\033[93m⚠ deep-translator 库未安装，仅为缓存模式（安装: pip install deep-translator）\033[0m")
    print()

    rows, cols = _get_win_size()

    try:
        pty = PtyProcess.spawn(args, env=env, cwd=os.getcwd(), dimensions=(rows, cols))
    except Exception as e:
        print(f"启动失败: {e}", file=sys.stderr)
        sys.exit(1)

    alive = True
    exit_code = 0
    lock = threading.Lock()

    # ── PTY → stdout (30ms 微缓冲 + 自动翻译) ──
    def reader():
        nonlocal alive, exit_code
        buf = ""
        last_flush = time.time()
        FRAME_SIGNALS = ('\n', '\x1b[H', '\x1b[2J', '\x1b[J')

        def do_flush():
            nonlocal buf, last_flush
            if buf:
                with lock:
                    sys.stdout.write(_translate(buf))
                    sys.stdout.flush()
                buf = ""
                last_flush = time.time()

        def has_frame_signal(s: str) -> bool:
            for sig in FRAME_SIGNALS:
                if sig in s:
                    return True
            return False

        while alive:
            try:
                data = pty.read(4096)
                now = time.time()
                if isinstance(data, bytes):
                    data = data.decode("utf-8", errors="replace")
                if data:
                    buf += data
                    if has_frame_signal(data):
                        do_flush()
                    elif now - last_flush > 0.03:
                        do_flush()
                    elif len(buf) > 8192:
                        do_flush()
                else:
                    do_flush()
                    time.sleep(0.005)
            except (OSError, EOFError):
                alive = False
                break
            except Exception:
                break
        do_flush()
        # drain
        try:
            while True:
                r = pty.read(4096)
                if not r:
                    break
                if isinstance(r, bytes):
                    r = r.decode("utf-8", errors="replace")
                sys.stdout.write(_translate(r))
                sys.stdout.flush()
        except Exception:
            pass
        _save_cache()
        try:
            exit_code = pty.wait()
        except Exception:
            exit_code = 0

    # ── stdin → PTY ──
    if sys.platform == "win32":
        import msvcrt
        def writer():
            nonlocal alive
            try:
                while alive:
                    if msvcrt.kbhit():
                        ch = msvcrt.getwch()
                        pty.write(ch)
                    else:
                        time.sleep(0.002)
            except (EOFError, OSError, KeyboardInterrupt):
                pass
    else:
        import select
        def writer():
            nonlocal alive
            try:
                while alive:
                    r, _, _ = select.select([sys.stdin], [], [], 0.1)
                    if r:
                        c = sys.stdin.read(1)
                        if not c:
                            break
                        pty.write(c)
            except (EOFError, OSError, KeyboardInterrupt):
                pass

    # ── 终端大小监控 ──
    def sizer():
        last = (0, 0)
        while alive:
            time.sleep(1.0)
            try:
                cur = _get_win_size()
                if cur != last and cur[0] > 0 and cur[1] > 0:
                    last = cur
                    pty.setwinsize(*cur)
            except Exception:
                pass

    rt = threading.Thread(target=reader, daemon=True, name="zh-r")
    wt = threading.Thread(target=writer, daemon=True, name="zh-w")
    st = threading.Thread(target=sizer, daemon=True, name="zh-sz")
    rt.start(); wt.start(); st.start()

    try:
        while alive:
            time.sleep(0.1)
    except KeyboardInterrupt:
        alive = False
        try:
            pty.write('\x03')
        except Exception:
            pass

    rt.join(timeout=3); wt.join(timeout=3); st.join(timeout=1)
    _save_cache()
    sys.exit(exit_code)

if __name__ == "__main__":
    main()
