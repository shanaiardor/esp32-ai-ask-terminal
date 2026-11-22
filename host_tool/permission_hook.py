#!/usr/bin/env python3
"""
Universal PreToolUse Permission Hook for ESP32 AI Ask-Me Terminal.

Compatible with:
  - Antigravity (~/.gemini/config/hooks.json or .agents/hooks.json)
  - Claude Code (~/.claude/settings.json PreToolUse)
  - Cursor (beforeShellExecution / hooks)

Modes:
  --mode smart   (default) Prompts on ESP32 for BypassSandbox, destructive commands,
                 network/package installs, git push/reset, flash/erase, etc.; auto-allows safe reads.
  --mode strict  Prompts on ESP32 for every matched tool invocation.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request

if hasattr(sys.stdin, "reconfigure"):
    sys.stdin.reconfigure(encoding="utf-8")
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

DEFAULT_HOST = os.environ.get("AI_TERMINAL_HOST", "192.168.3.152")

RISKY_PATTERNS = (
    "rm ", "rm\t", "rmdir", "del ", "erase ", "remove-item", "format ",
    "git push", "git reset", "git clean", "git checkout --",
    "pip install", "pip uninstall", "npm install", "npm i ", "pnpm add", "yarn add",
    "idf.py flash", "idf.py erase", "esptool", "curl ", "wget ", "invoke-webrequest",
    "sudo ", "chmod ", "chown ", "reg add", "reg delete", "kill ", "stop-process",
)


def ask_esp32(host: str, title: str, subtitle: str, kv: list[dict], timeout_sec: int = 60) -> str:
    payload = {
        "wait": True,
        "state": "warning",
        "agent": "Security Gate",
        "title": title[:90],
        "subtitle": subtitle[:60],
        "layout": "kv",
        "kv": kv[:4],
        "buttons": [
            {"id": "allow", "label": "允许执行", "style": "success"},
            {"id": "ask_ui", "label": "转桌面弹窗", "style": "primary"},
            {"id": "deny", "label": "拒绝拦截", "style": "danger", "is_default": True},
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
        res = json.loads(resp.read().decode("utf-8"))
        return str(res.get("action_id") or "deny")


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32 Terminal PreToolUse Permission Hook")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--mode", choices=["smart", "strict"], default=os.environ.get("AI_TERMINAL_HOOK_MODE", "smart"))
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()

    raw = sys.stdin.read().strip()
    if not raw:
        print(json.dumps({"decision": "allow"}))
        return

    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        print(json.dumps({"decision": "ask", "reason": "Hook received non-JSON payload"}))
        return

    tc = data.get("toolCall") if isinstance(data.get("toolCall"), dict) else {}
    tool_name = str(tc.get("name") or data.get("tool_name") or "tool")
    tool_args = tc.get("args") if isinstance(tc.get("args"), dict) else (data.get("tool_input") or {})

    cmd = str(tool_args.get("CommandLine") or tool_args.get("command") or "")
    target_path = str(tool_args.get("Path") or tool_args.get("file_path") or tool_args.get("TargetFile") or "")
    bypass = bool(tool_args.get("BypassSandbox", False))

    # Never intercept calls to the terminal itself
    if "ai-ask-terminal" in tool_name or "ai_terminal_mcp" in cmd or "ask_cli.py" in cmd:
        print(json.dumps({"decision": "allow"}))
        return

    cmd_lower = cmd.lower()
    is_risky = bypass or any(pat in cmd_lower for pat in RISKY_PATTERNS)
    if args.mode == "smart" and not is_risky and tool_name in ("run_command", "Bash", "bash", "execute_command"):
        print(json.dumps({"decision": "allow"}))
        return

    kv = [
        {"k": "工具", "v": tool_name[:28], "highlight": True},
        {"k": "命令/目标", "v": (cmd or target_path or json.dumps(tool_args, ensure_ascii=False))[:60]},
        {"k": "沙箱绕过", "v": "是 (BypassSandbox)" if bypass else "否", "highlight": bypass},
        {"k": "工作区", "v": str(data.get("cwd") or "")[-42:] or "默认目录"},
    ]

    try:
        action = ask_esp32(
            host=args.host,
            title=f"权限审批: {tool_name[:26]}",
            subtitle=f"{args.mode.upper()} 安全网关 · 请在屏幕确认",
            kv=kv,
            timeout_sec=args.timeout,
        )
    except (urllib.error.URLError, TimeoutError, OSError):
        # Fallback cleanly to host desktop UI if ESP32 is offline
        print(json.dumps({"decision": "ask", "reason": "ESP32 terminal unreachable, fallback to desktop prompt"}))
        return

    if action == "allow":
        print(json.dumps({"decision": "allow", "reason": "Approved on ESP32 Ask-Me Terminal"}))
    elif action == "ask_ui":
        print(json.dumps({"decision": "ask", "reason": "Delegated to desktop UI from ESP32 Terminal"}))
    else:
        print(json.dumps({"decision": "deny", "reason": "Denied on ESP32 Ask-Me Terminal"}))


if __name__ == "__main__":
    main()
