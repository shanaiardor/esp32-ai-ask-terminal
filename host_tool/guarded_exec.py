#!/usr/bin/env python3
"""
Generic Guarded Command Wrapper for ANY AI Agent (Aider, OpenCode, LangChain, Bash-only Agents).

Usage:
  python guarded_exec.py --host 192.168.3.152 -- git push origin main
  python guarded_exec.py --mode strict -- npm run deploy

Behavior:
  - In `smart` mode (default): safe commands run directly; risky commands trigger
    an interactive approval card + warning chime on the ESP32 touch screen first.
  - In `strict` mode: every command prompts on the ESP32 touch screen before execution.
  - If approved on the screen, runs the command and forwards exit code + stdout/stderr.
  - If denied or timed out, aborts with exit code 126 without running the command.
"""

import argparse
import json
import os
import shlex
import subprocess
import sys
import urllib.error
import urllib.request

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

DEFAULT_HOST = os.environ.get("AI_TERMINAL_HOST", "192.168.3.152")

RISKY_PATTERNS = (
    "rm ", "rm\t", "rmdir", "del ", "erase ", "remove-item", "format ",
    "git push", "git reset", "git clean", "git checkout --",
    "pip install", "pip uninstall", "npm install", "npm i ", "pnpm add", "yarn add",
    "idf.py flash", "idf.py erase", "esptool", "curl ", "wget ",
    "sudo ", "chmod ", "chown ", "reg ", "kill ", "stop-process",
)


def request_approval(host: str, cmd_str: str, cwd: str, timeout_sec: int) -> dict:
    payload = {
        "wait": True,
        "state": "warning",
        "agent": "Guarded Shell",
        "title": "通用命令执行权限审批",
        "subtitle": "请在 ESP32 触摸屏确认放行或拦截",
        "layout": "kv",
        "kv": [
            {"k": "命令", "v": cmd_str[:62], "highlight": True},
            {"k": "目录", "v": cwd[-44:] or "."},
            {"k": "超时", "v": f"{timeout_sec} 秒默认拒绝"},
        ],
        "buttons": [
            {"id": "allow", "label": "允许执行", "style": "success"},
            {"id": "deny", "label": "拒绝执行", "style": "danger", "is_default": True},
        ],
        "timeout_sec": timeout_sec,
        "sound": "warning",
    }
    req = urllib.request.Request(
        f"http://{host}/api/ask",
        data=json.dumps(payload, ensure_ascii=True).encode("ascii"),
        headers={"Content-Type": "application/json; charset=utf-8"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout_sec + 5) as resp:
        return json.loads(resp.read().decode("utf-8"))


def main() -> None:
    parser = argparse.ArgumentParser(description="Execute shell command gated by ESP32 Ask-Me Terminal")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--mode", choices=["smart", "strict"], default="smart")
    parser.add_argument("--timeout", type=int, default=60)
    parser.add_argument("cmd", nargs=argparse.REMAINDER, help="Command to execute (prefix with --)")
    args = parser.parse_args()

    cmd_list = list(args.cmd)
    if cmd_list and cmd_list[0] == "--":
        cmd_list = cmd_list[1:]
    if not cmd_list:
        print("Usage: python guarded_exec.py -- <command...>", file=sys.stderr)
        sys.exit(2)

    cmd_str = subprocess.list2cmdline(cmd_list) if sys.platform == "win32" else shlex.join(cmd_list)
    cmd_lower = cmd_str.lower()
    is_risky = any(p in cmd_lower for p in RISKY_PATTERNS)

    if args.mode == "strict" or is_risky:
        try:
            res = request_approval(args.host, cmd_str, os.getcwd(), args.timeout)
        except (urllib.error.URLError, TimeoutError, OSError) as exc:
            print(f"[GuardedExec] ESP32 terminal unreachable ({exc}); aborting risky command.", file=sys.stderr)
            sys.exit(126)

        if res.get("action_id") != "allow":
            print(f"[GuardedExec] Denied on ESP32 terminal: {res}", file=sys.stderr)
            sys.exit(126)

    if len(cmd_list) == 1:
        proc = subprocess.run(cmd_list[0], shell=True)
    else:
        proc = subprocess.run(cmd_list, shell=False)
    sys.exit(proc.returncode)


if __name__ == "__main__":
    main()
