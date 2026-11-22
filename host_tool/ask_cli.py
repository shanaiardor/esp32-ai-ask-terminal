#!/usr/bin/env python3
"""
CLI helper for calling the ESP32 AI Ask-Me Terminal from any shell or AI tool.

Examples:
  python ask_cli.py --host 192.168.1.50 state
  python ask_cli.py --host 192.168.1.50 sound --preset ask
  python ask_cli.py --host 192.168.1.50 status --state running --title "正在构建项目" --percent 65
  python ask_cli.py --host 192.168.1.50 ask --title "是否提交代码？" --layout kv --kv "分支=main,变更=5文件" --buttons "ok:确认提交:success:default,cancel:取消:danger"
"""

import argparse
import json
import os
import sys
import urllib.request


def post_json(host: str, path: str, data: dict, timeout: float = 120.0) -> dict:
    req = urllib.request.Request(
        f"http://{host}{path}",
        data=json.dumps(data, ensure_ascii=True).encode("ascii"),
        headers={"Content-Type": "application/json; charset=utf-8"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def get_json(host: str, path: str, timeout: float = 10.0) -> dict:
    req = urllib.request.Request(f"http://{host}{path}", method="GET")
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def parse_buttons(spec: str) -> list[dict]:
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        segs = part.split(":")
        btn = {"id": segs[0], "label": segs[1] if len(segs) > 1 else segs[0]}
        if len(segs) > 2 and segs[2]:
            btn["style"] = segs[2]
        if len(segs) > 3 and segs[3] == "default":
            btn["is_default"] = True
        out.append(btn)
        if len(out) >= 4:
            break
    return out


def parse_kv(spec: str) -> list[dict]:
    out = []
    for part in spec.split(","):
        if "=" not in part:
            continue
        k, v = part.split("=", 1)
        hl = k.startswith("*")
        if hl:
            k = k[1:]
        out.append({"k": k.strip(), "v": v.strip(), "highlight": hl})
        if len(out) >= 4:
            break
    return out


def parse_choices(spec: str) -> list[dict]:
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        segs = part.split(":")
        sel = len(segs) > 2 and segs[2].lower() in ("1", "true", "sel", "selected")
        out.append({
            "id": segs[0],
            "label": segs[1] if len(segs) > 1 else segs[0],
            "selected": sel,
        })
        if len(out) >= 6:
            break
    return out


def main() -> None:
    parser = argparse.ArgumentParser(description="ESP32 AI Ask-Me Terminal CLI")
    parser.add_argument("--host", default=os.environ.get("AI_TERMINAL_HOST", "192.168.4.1"), help="ESP32 IP")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("state", help="Get terminal state")
    sub.add_parser("dismiss", help="Dismiss current screen")

    p_snd = sub.add_parser("sound", help="Play sound or set volume")
    p_snd.add_argument("--preset", default="ask")
    p_snd.add_argument("--melody", default="")
    p_snd.add_argument("--repeat", type=int, default=1)
    p_snd.add_argument("--volume", type=int, default=None)
    p_snd.add_argument("--mute", choices=["true", "false"], default=None)

    p_st = sub.add_parser("status", help="Push async status screen")
    p_st.add_argument("--state", default="running", choices=["idle", "thinking", "asking", "running", "success", "warning", "error"])
    p_st.add_argument("--agent", default="AI Agent")
    p_st.add_argument("--title", required=True)
    p_st.add_argument("--subtitle", default="")
    p_st.add_argument("--layout", default="text", choices=["text", "kv", "progress"])
    p_st.add_argument("--body", default="")
    p_st.add_argument("--callout", default="")
    p_st.add_argument("--kv", default="")
    p_st.add_argument("--percent", type=int, default=None)
    p_st.add_argument("--step-text", default="")
    p_st.add_argument("--sound", default="info")

    p_ask = sub.add_parser("ask", help="Prompt user and wait for button click")
    p_ask.add_argument("--state", default="asking", choices=["idle", "thinking", "asking", "running", "success", "warning", "error"])
    p_ask.add_argument("--agent", default="AI Agent")
    p_ask.add_argument("--title", required=True)
    p_ask.add_argument("--subtitle", default="")
    p_ask.add_argument("--layout", default="text", choices=["text", "kv", "choice", "number", "progress"])
    p_ask.add_argument("--body", default="")
    p_ask.add_argument("--callout", default="")
    p_ask.add_argument("--kv", default="")
    p_ask.add_argument("--choices", default="")
    p_ask.add_argument("--multi", action="store_true")
    p_ask.add_argument("--value", type=int, default=10)
    p_ask.add_argument("--min", type=int, default=0)
    p_ask.add_argument("--max", type=int, default=100)
    p_ask.add_argument("--step", type=int, default=1)
    p_ask.add_argument("--unit", default="")
    p_ask.add_argument("--percent", type=int, default=None)
    p_ask.add_argument("--step-text", default="")
    p_ask.add_argument("--buttons", default="confirm:确认:primary:default,cancel:取消:secondary")
    p_ask.add_argument("--timeout", type=int, default=60)
    p_ask.add_argument("--sound", default="ask")
    p_ask.add_argument("--no-wait", action="store_true")

    args = parser.parse_args()

    if args.cmd == "state":
        res = get_json(args.host, "/api/state")
    elif args.cmd == "dismiss":
        res = post_json(args.host, "/api/dismiss", {})
    elif args.cmd == "sound":
        payload = {"repeat": args.repeat}
        if args.melody:
            payload["melody"] = args.melody
        elif args.preset:
            payload["preset"] = args.preset
        if args.volume is not None:
            payload["volume"] = args.volume
        if args.mute is not None:
            payload["mute"] = args.mute == "true"
        res = post_json(args.host, "/api/sound", payload)
    elif args.cmd == "status":
        payload = {
            "state": args.state,
            "agent": args.agent,
            "title": args.title,
            "subtitle": args.subtitle,
            "layout": args.layout,
            "body": args.body,
            "callout": args.callout,
            "sound": args.sound,
        }
        if args.kv:
            payload["kv"] = parse_kv(args.kv)
        if args.percent is not None:
            payload["layout"] = "progress"
            payload["percent"] = args.percent
            payload["step_text"] = args.step_text or args.body
        res = post_json(args.host, "/api/status", payload)
    elif args.cmd == "ask":
        payload = {
            "state": args.state,
            "agent": args.agent,
            "title": args.title,
            "subtitle": args.subtitle,
            "layout": args.layout,
            "body": args.body,
            "callout": args.callout,
            "buttons": parse_buttons(args.buttons),
            "timeout_sec": args.timeout,
            "sound": args.sound,
            "wait": not args.no_wait,
        }
        if args.kv:
            payload["kv"] = parse_kv(args.kv)
        if args.choices:
            payload["choices"] = parse_choices(args.choices)
            payload["multi"] = args.multi
        if args.layout == "number":
            payload["value"] = args.value
            payload["min"] = args.min
            payload["max"] = args.max
            payload["step"] = args.step
            payload["unit"] = args.unit
        if args.percent is not None:
            payload["percent"] = args.percent
        if args.step_text:
            payload["step_text"] = args.step_text
        res = post_json(args.host, "/api/ask", payload, timeout=max(15.0, args.timeout + 10.0))
    else:
        sys.exit(1)

    print(json.dumps(res, ensure_ascii=False, indent=2))


if __name__ == "__main__":
    main()
