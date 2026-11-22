#!/usr/bin/env python3
"""
MCP (Model Context Protocol) stdio JSON-RPC Server for ESP32 AI Ask-Me Terminal.

Exposes structured, DSL-constrained tools so AI agents (Claude Code, Cursor, Antigravity, etc.)
can play notification sounds, display status updates, and ask users interactive questions
with fixed button slots on the 640x172 touch screen.

Environment variables:
  AI_TERMINAL_HOST  IP or hostname of the ESP32 terminal (default: 192.168.4.1)
"""

import json
import os
import sys
import urllib.error
import urllib.request

DEFAULT_HOST = os.environ.get("AI_TERMINAL_HOST", "192.168.3.152")

if hasattr(sys.stdin, "reconfigure"):
    sys.stdin.reconfigure(encoding="utf-8")
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")


def _http_post(host: str, path: str, payload: dict, timeout_sec: float = 120.0) -> dict:
    url = f"http://{host}{path}"
    data = json.dumps(payload, ensure_ascii=True).encode("ascii")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json; charset=utf-8"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
        return json.loads(resp.read().decode("utf-8"))


def _http_get(host: str, path: str, timeout_sec: float = 10.0) -> dict:
    url = f"http://{host}{path}"
    req = urllib.request.Request(url, method="GET")
    with urllib.request.urlopen(req, timeout=timeout_sec) as resp:
        return json.loads(resp.read().decode("utf-8"))


TOOLS = [
    {
        "name": "ask_user_on_terminal",
        "description": (
            "Show a structured DSL prompt on the ESP32-S3 640x172 touch terminal, play an alert sound, "
            "and block until the user taps one of the 1..4 fixed-slot buttons (or timeout). "
            "Layout is constrained to a 3-zone grid (Status Dock | Structured Body | 1..4 Right Action Buttons)."
        ),
        "inputSchema": {
            "type": "object",
            "required": ["title", "layout", "buttons"],
            "properties": {
                "host": {"type": "string", "description": "Optional ESP32 IP override (defaults to AI_TERMINAL_HOST)."},
                "state": {
                    "type": "string",
                    "enum": ["idle", "thinking", "asking", "running", "success", "warning", "error"],
                    "default": "asking",
                },
                "agent": {"type": "string", "description": "Short AI agent label (max 20 chars).", "default": "AI Agent"},
                "title": {"type": "string", "description": "Main prompt title (single line, concise)."},
                "subtitle": {"type": "string", "description": "Optional context subtitle."},
                "layout": {
                    "type": "string",
                    "enum": ["text", "kv", "choice", "number", "progress"],
                    "description": "Constrained center-zone template.",
                },
                "body": {"type": "string", "description": "For layout=text: wrapped description body."},
                "callout": {"type": "string", "description": "For layout=text: highlighted bottom warning/code strip."},
                "kv": {
                    "type": "array",
                    "maxItems": 4,
                    "description": "For layout=kv: up to 4 key-value rows.",
                    "items": {
                        "type": "object",
                        "required": ["k", "v"],
                        "properties": {
                            "k": {"type": "string"},
                            "v": {"type": "string"},
                            "highlight": {"type": "boolean"},
                        },
                    },
                },
                "choices": {
                    "type": "array",
                    "maxItems": 6,
                    "description": "For layout=choice: up to 6 selectable capsule chips.",
                    "items": {
                        "type": "object",
                        "required": ["id", "label"],
                        "properties": {
                            "id": {"type": "string"},
                            "label": {"type": "string"},
                            "selected": {"type": "boolean"},
                        },
                    },
                },
                "multi": {"type": "boolean", "description": "For layout=choice: allow multi-select.", "default": False},
                "value": {"type": "integer", "description": "For layout=number: initial value."},
                "min": {"type": "integer", "description": "For layout=number: minimum value."},
                "max": {"type": "integer", "description": "For layout=number: maximum value."},
                "step": {"type": "integer", "description": "For layout=number: step increment."},
                "unit": {"type": "string", "description": "For layout=number: short unit suffix."},
                "percent": {"type": "integer", "minimum": 0, "maximum": 100, "description": "For layout=progress: 0..100 percentage."},
                "step_text": {"type": "string", "description": "For layout=progress: current step status text."},
                "steps": {
                    "type": "array",
                    "maxItems": 4,
                    "description": "For layout=progress: up to 4 pipeline steps.",
                    "items": {
                        "type": "object",
                        "required": ["label"],
                        "properties": {
                            "label": {"type": "string"},
                            "done": {"type": "boolean"},
                            "active": {"type": "boolean"},
                        },
                    },
                },
                "buttons": {
                    "type": "array",
                    "minItems": 1,
                    "maxItems": 4,
                    "description": "Strictly 1..4 fixed-position right-dock buttons.",
                    "items": {
                        "type": "object",
                        "required": ["id", "label"],
                        "properties": {
                            "id": {"type": "string"},
                            "label": {"type": "string"},
                            "style": {
                                "type": "string",
                                "enum": ["primary", "success", "danger", "warning", "secondary"],
                            },
                            "is_default": {"type": "boolean"},
                        },
                    },
                },
                "timeout_sec": {"type": "integer", "minimum": 5, "maximum": 600, "default": 60},
                "sound": {
                    "oneOf": [
                        {"type": "string", "enum": ["none", "info", "ask", "success", "warning", "error", "urgent"]},
                        {
                            "type": "object",
                            "properties": {
                                "preset": {"type": "string"},
                                "melody": {"type": "string", "description": "e.g. 'C5:80,E5:80,G5:150'"},
                                "repeat": {"type": "integer", "minimum": 1, "maximum": 5},
                            },
                        },
                    ],
                    "default": "ask",
                },
            },
        },
    },
    {
        "name": "show_ai_status_on_terminal",
        "description": (
            "Push an asynchronous status screen (thinking / running / progress / success / warning / error) "
            "with optional sound to the ESP32 terminal without blocking."
        ),
        "inputSchema": {
            "type": "object",
            "required": ["state", "title"],
            "properties": {
                "host": {"type": "string"},
                "state": {
                    "type": "string",
                    "enum": ["idle", "thinking", "asking", "running", "success", "warning", "error"],
                },
                "agent": {"type": "string", "default": "AI Agent"},
                "title": {"type": "string"},
                "subtitle": {"type": "string"},
                "layout": {"type": "string", "enum": ["text", "kv", "progress"], "default": "text"},
                "body": {"type": "string"},
                "callout": {"type": "string"},
                "kv": {
                    "type": "array",
                    "maxItems": 4,
                    "description": "For layout=kv: up to 4 key-value rows.",
                    "items": {
                        "type": "object",
                        "required": ["k", "v"],
                        "properties": {
                            "k": {"type": "string"},
                            "v": {"type": "string"},
                            "highlight": {"type": "boolean"},
                        },
                    },
                },
                "percent": {"type": "integer", "minimum": 0, "maximum": 100},
                "step_text": {"type": "string"},
                "steps": {
                    "type": "array",
                    "maxItems": 4,
                    "items": {
                        "type": "object",
                        "required": ["label"],
                        "properties": {
                            "label": {"type": "string"},
                            "done": {"type": "boolean"},
                            "active": {"type": "boolean"},
                        },
                    },
                },
                "sound": {"type": "string", "enum": ["none", "info", "ask", "success", "warning", "error", "urgent"]},
            },
        },
    },
    {
        "name": "play_terminal_sound",
        "description": "Play a preset chime or custom note melody on the ESP32 ES8311 speaker.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "host": {"type": "string"},
                "preset": {"type": "string", "enum": ["info", "ask", "success", "warning", "error", "urgent", "click"]},
                "melody": {"type": "string", "description": "Note sequence like 'C5:80,E5:80,G5:120,C6:180'"},
                "repeat": {"type": "integer", "minimum": 1, "maximum": 5, "default": 1},
                "volume": {"type": "integer", "minimum": 0, "maximum": 100},
                "mute": {"type": "boolean"},
            },
        },
    },
    {
        "name": "dismiss_terminal_prompt",
        "description": "Dismiss the active prompt/status on the ESP32 terminal and return to the idle dashboard.",
        "inputSchema": {
            "type": "object",
            "properties": {
                "host": {"type": "string"},
            },
        },
    },
    {
        "name": "approve_tool_permission",
        "description": (
            "Permission prompt handler for AI agents (compatible with Claude Code --permission-prompt-tool). "
            "Displays the tool name and key arguments on the ESP32 terminal, plays a warning chime, "
            "blocks for user tap, and returns allow/deny decision JSON."
        ),
        "inputSchema": {
            "type": "object",
            "required": ["tool_name"],
            "properties": {
                "host": {"type": "string"},
                "tool_name": {"type": "string"},
                "input": {"type": "object", "description": "Tool input arguments object."},
                "tool_input": {"type": "object", "description": "Alias for input arguments."},
                "reason": {"type": "string", "description": "Optional reason why permission is requested."},
            },
        },
    },
]


def _handle_permission_prompt(host: str, args: dict) -> dict:
    tool_name = str(args.get("tool_name") or "Tool")
    tool_input = args.get("input") if isinstance(args.get("input"), dict) else (args.get("tool_input") or {})
    reason = str(args.get("reason") or "Agent 请求执行工具权限")

    summary_val = ""
    for key in ("CommandLine", "command", "Path", "file_path", "path", "url"):
        if key in tool_input and tool_input[key]:
            summary_val = str(tool_input[key])
            break
    if not summary_val and tool_input:
        summary_val = json.dumps(tool_input, ensure_ascii=False)

    kv = [
        {"k": "工具", "v": tool_name[:30], "highlight": True},
        {"k": "参数", "v": (summary_val or "(无参数)")[:62]},
        {"k": "说明", "v": reason[:60]},
    ]
    payload = {
        "wait": True,
        "state": "warning",
        "agent": "Permission Gate",
        "title": f"权限审批：{tool_name[:28]}",
        "subtitle": "请在物理屏幕上确认放行或拒绝",
        "layout": "kv",
        "kv": kv,
        "buttons": [
            {"id": "allow", "label": "允许执行", "style": "success"},
            {"id": "deny", "label": "拒绝执行", "style": "danger", "is_default": True},
        ],
        "timeout_sec": 60,
        "sound": "warning",
    }
    res = _http_post(host, "/api/ask", payload, timeout_sec=70.0)
    allowed = res.get("action_id") == "allow" and res.get("status") == "answered"
    if allowed:
        return {
            "ok": True,
            "behavior": "allow",
            "updatedInput": tool_input,
            "terminal_result": res,
        }
    return {
        "ok": True,
        "behavior": "deny",
        "message": f"User denied {tool_name} on ESP32 terminal ({res.get('status')})",
        "terminal_result": res,
    }


def handle_tool_call(name: str, args: dict) -> dict:
    host = args.pop("host", None) or DEFAULT_HOST
    if name == "ask_user_on_terminal":
        to_sec = int(args.get("timeout_sec", 60))
        args["wait"] = True
        return _http_post(host, "/api/ask", args, timeout_sec=max(15.0, to_sec + 10.0))
    if name == "show_ai_status_on_terminal":
        return _http_post(host, "/api/status", args, timeout_sec=10.0)
    if name == "play_terminal_sound":
        return _http_post(host, "/api/sound", args, timeout_sec=10.0)
    if name == "dismiss_terminal_prompt":
        return _http_post(host, "/api/dismiss", {}, timeout_sec=10.0)
    if name == "approve_tool_permission":
        return _handle_permission_prompt(host, args)
    raise ValueError(f"Unknown tool: {name}")


def write_msg(obj: dict) -> None:
    line = json.dumps(obj, ensure_ascii=False)
    sys.stdout.write(line + "\n")
    sys.stdout.flush()


def main() -> None:
    global DEFAULT_HOST
    for i, arg in enumerate(sys.argv[1:]):
        if arg == "--host" and i + 2 < len(sys.argv):
            DEFAULT_HOST = sys.argv[i + 2]

    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            msg = json.loads(raw)
        except json.JSONDecodeError:
            continue

        msg_id = msg.get("id")
        method = msg.get("method")
        params = msg.get("params") or {}

        if method == "initialize":
            write_msg({
                "jsonrpc": "2.0",
                "id": msg_id,
                "result": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "ai-ask-terminal", "version": "1.0.0"},
                },
            })
        elif method == "notifications/initialized":
            continue
        elif method == "tools/list":
            write_msg({"jsonrpc": "2.0", "id": msg_id, "result": {"tools": TOOLS}})
        elif method == "tools/call":
            tname = params.get("name", "")
            targs = dict(params.get("arguments") or {})
            try:
                res = handle_tool_call(tname, targs)
                write_msg({
                    "jsonrpc": "2.0",
                    "id": msg_id,
                    "result": {
                        "content": [{"type": "text", "text": json.dumps(res, ensure_ascii=False, indent=2)}],
                        "isError": not res.get("ok", True),
                    },
                })
            except (urllib.error.URLError, TimeoutError, Exception) as exc:
                write_msg({
                    "jsonrpc": "2.0",
                    "id": msg_id,
                    "result": {
                        "content": [{"type": "text", "text": f"Terminal request failed: {exc}"}],
                        "isError": True,
                    },
                })
        elif msg_id is not None:
            write_msg({
                "jsonrpc": "2.0",
                "id": msg_id,
                "error": {"code": -32601, "message": f"Method not found: {method}"},
            })


if __name__ == "__main__":
    main()
