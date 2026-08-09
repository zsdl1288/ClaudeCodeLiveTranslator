#!/usr/bin/env python3
"""
Claude Code CLI 中文化包装器 v2.1
直接在终端运行，实时翻译 Claude Code 输出中的英文 UI 文字。

用法:  python claude-zh.py [参数...]
"""

import sys
import os
import json
import re
import time
import threading
import atexit

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
# 加载翻译表
# ═══════════════════════════════════════════════════════════════
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
_TRANS_PATH = os.path.join(_SCRIPT_DIR, "translations.json")

def _load_translations():
    if not os.path.exists(_TRANS_PATH):
        return [], []
    with open(_TRANS_PATH, "r", encoding="utf-8") as f:
        data = json.load(f)
    phrases = []
    for eng, chn in data.get("phrases", []):
        phrases.append((eng, chn))
    phrases.sort(key=lambda x: -len(x[0]))
    regex_rules = []
    for pat, rep in data.get("regex_patterns", []):
        try:
            regex_rules.append((re.compile(pat), rep))
        except Exception:
            pass
    return phrases, regex_rules

_PHRASES, _REGEXES = _load_translations()

def _translate(text: str) -> str:
    """直接替换。英文词不可能出现在 ANSI 转义序列中。"""
    if not text:
        return ""
    for eng, chn in _PHRASES:
        if eng in text:
            text = text.replace(eng, chn)
    for pat, rep in _REGEXES:
        text = pat.sub(rep, text)
    return text

# 调试：把所有原始输出记入日志
def _debug_raw(text: str):
    try:
        with open(os.path.join(_SCRIPT_DIR, "raw_output.log"), "a", encoding="utf-8") as f:
            f.write("---CHUNK---\n")
            f.write(text)
            f.write("\n")
    except Exception:
        pass

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

    # 启用 VT 处理
    _ENABLE_VT = 0x0004
    _out_mode = ctypes.c_uint32()
    _kernel32.GetConsoleMode(_h_out, ctypes.byref(_out_mode))
    _kernel32.SetConsoleMode(_h_out, _out_mode.value | _ENABLE_VT)

    def _get_win_size():
        """正确解析 CONSOLE_SCREEN_BUFFER_INFO 获取终端大小。
        CSBI struct layout (22 bytes):
          0: dwSize.X,  2: dwSize.Y
          4: dwCursor.X, 6: dwCursor.Y
          8: wAttributes
          10: srWindow.Left, 12: srWindow.Top
          14: srWindow.Right, 16: srWindow.Bottom
          18: dwMaxWinSize.X, 20: dwMaxWinSize.Y
        """
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

    print("\033[1;36mClaude Code 中文化 v2.1\033[0m")
    print("\033[90m词条 {} 条\033[0m".format(len(_PHRASES)))

    rows, cols = _get_win_size()

    try:
        pty = PtyProcess.spawn(args, env=env, cwd=os.getcwd(), dimensions=(rows, cols))
    except Exception as e:
        print(f"启动失败: {e}", file=sys.stderr)
        sys.exit(1)

    alive = True
    exit_code = 0
    lock = threading.Lock()

    # ── PTY → stdout ──
    def reader():
        nonlocal alive, exit_code
        buf = ""
        while alive:
            try:
                data = pty.read(4096)
                if isinstance(data, bytes):
                    data = data.decode("utf-8", errors="replace")
                if data:
                    # 积累碎片
                    buf += data
                    # 屏幕刷新或够大了就 flush
                    if '\x1b[H' in buf or '\x1b[2J' in buf or len(buf) > 4096 or '\n' in buf[-60:]:
                        # _debug_raw(buf)
                        with lock:
                            sys.stdout.write(_translate(buf))
                            sys.stdout.flush()
                        buf = ""
                else:
                    # 无数据 → flush 残留 + 短暂休眠
                    if buf:
                        # _debug_raw(buf)
                        with lock:
                            sys.stdout.write(_translate(buf))
                            sys.stdout.flush()
                        buf = ""
                    time.sleep(0.01)
            except (OSError, EOFError):
                alive = False
                break
            except Exception:
                break
        # 最后的缓冲
        if buf:
            sys.stdout.write(_translate(buf))
            sys.stdout.flush()
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
                        ch = msvcrt.getwch()  # 宽字符模式，支持中文 IME
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
    sys.exit(exit_code)

if __name__ == "__main__":
    main()
