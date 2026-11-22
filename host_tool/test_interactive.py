#!/usr/bin/env python3
"""
Interactive hardware test suite for ESP32 AI Ask-Me Terminal.

Run with:
  python test_interactive.py --host 192.168.3.152
  python test_interactive.py --host 192.168.3.152 --all
  python test_interactive.py --host 192.168.3.152 --check
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request

DEFAULT_HOST = os.environ.get("AI_TERMINAL_HOST", "192.168.3.152")


def req_json(host: str, path: str, payload: dict | None = None, timeout: float = 120.0) -> dict:
    url = f"http://{host}{path}"
    if payload is None:
        req = urllib.request.Request(url, method="GET")
    else:
        req = urllib.request.Request(
            url,
            data=json.dumps(payload, ensure_ascii=False).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return json.loads(resp.read().decode("utf-8"))


def print_result(title: str, res: dict) -> None:
    print(f"\n>>> [{title}] 终端返回结果:")
    print(json.dumps(res, ensure_ascii=False, indent=2))


def test_sound(host: str) -> None:
    print("\n[测试 1] 正在依次播放预设提示音与自定义旋律...")
    for preset in ("info", "ask", "warning", "error", "success"):
        print(f"  - 播放预设音效: {preset}")
        req_json(host, "/api/sound", {"preset": preset}, timeout=10)
        time.sleep(0.75)

    melody = "E5:90,E5:90,R:80,E5:90,R:80,C5:90,E5:140,G5:200,R:120,G4:220"
    print(f"  - 播放自定义音符旋律: {melody}")
    req_json(host, "/api/sound", {"melody": melody, "repeat": 1}, timeout=10)
    time.sleep(1.5)
    print("  [完成] 音效测试结束")


def test_text_ask(host: str) -> None:
    print("\n[测试 2] 已在屏幕上弹出 [text 富文本 + 3 按钮]，请在触摸屏上点击任意按钮 (60s 超时)...")
    res = req_json(host, "/api/ask", {
        "state": "asking",
        "agent": "Claude Code",
        "title": "是否执行数据库迁移脚本？",
        "subtitle": "prisma migrate deploy · 2 个待执行变更",
        "layout": "text",
        "body": "即将修改 users 表索引并追加审计字段，建议先确认当前无长事务阻塞。",
        "callout": "警告：操作期间将短暂锁定 users 表约 3 秒",
        "buttons": [
            {"id": "approve", "label": "立即执行", "style": "success"},
            {"id": "preview", "label": "仅预览 SQL", "style": "primary", "is_default": True},
            {"id": "cancel", "label": "取消操作", "style": "danger"},
        ],
        "timeout_sec": 60,
        "sound": "ask",
    })
    print_result("测试 2: text + 3 按钮", res)


def test_kv_ask(host: str) -> None:
    print("\n[测试 3] 已在屏幕上弹出 [kv 键值对审查 + 2 按钮]，请在触摸屏上点击按钮...")
    res = req_json(host, "/api/ask", {
        "state": "warning",
        "agent": "Codex CLI",
        "title": "高危 Shell 命令执行审查",
        "subtitle": "安全策略拦截 · 等待人工放行",
        "layout": "kv",
        "kv": [
            {"k": "执行命令", "v": "rm -rf ./dist && npm run build", "highlight": True},
            {"k": "工作目录", "v": "/srv/app-backend"},
            {"k": "影响文件", "v": "142 个构建产物"},
            {"k": "预估耗时", "v": "约 18 秒"},
        ],
        "buttons": [
            {"id": "allow", "label": "允许执行", "style": "success"},
            {"id": "deny", "label": "拒绝执行", "style": "danger", "is_default": True},
        ],
        "timeout_sec": 60,
        "sound": "warning",
    })
    print_result("测试 3: kv 参数审查", res)


def test_choice_ask(host: str) -> None:
    print("\n[测试 4] 已在屏幕上弹出 [choice 多选胶囊]，请在屏幕中栏点选选项后，点击右侧按钮提交...")
    res = req_json(host, "/api/ask", {
        "state": "asking",
        "agent": "Antigravity",
        "title": "请选择要运行的自动化测试套件",
        "subtitle": "支持多选：点选下方胶囊后点击右侧提交",
        "layout": "choice",
        "multi": True,
        "choices": [
            {"id": "unit", "label": "单元测试", "selected": True},
            {"id": "api", "label": "接口集成", "selected": True},
            {"id": "e2e", "label": "端到端 UI", "selected": False},
            {"id": "perf", "label": "性能压测", "selected": False},
        ],
        "buttons": [
            {"id": "run_selected", "label": "运行所选", "style": "primary", "is_default": True},
            {"id": "run_all", "label": "全部运行", "style": "success"},
            {"id": "skip", "label": "跳过", "style": "secondary"},
        ],
        "timeout_sec": 60,
        "sound": "info",
    })
    print_result("测试 4: choice 多选胶囊", res)


def test_number_ask(host: str) -> None:
    print("\n[测试 5] 已在屏幕上弹出 [number 数值调节器]，请用屏幕 [-] [+] 调整数值后点击右侧按钮...")
    res = req_json(host, "/api/ask", {
        "state": "asking",
        "agent": "AutoBuilder",
        "title": "请设定并行编译线程数",
        "subtitle": "点击中栏 - / + 微调后点击右侧应用",
        "layout": "number",
        "value": 8,
        "min": 2,
        "max": 32,
        "step": 2,
        "unit": "核",
        "buttons": [
            {"id": "apply", "label": "应用设定", "style": "primary", "is_default": True},
            {"id": "default", "label": "恢复默认", "style": "secondary"},
        ],
        "timeout_sec": 60,
        "sound": "info",
    })
    print_result("测试 5: number 数值微调", res)


def test_progress_flow(host: str) -> None:
    print("\n[测试 6] 正在模拟 AI 任务多阶段进度推送 (/api/status)...")
    stages = [
        ("thinking", "正在分析代码依赖树...", 15, [
            {"label": "分析", "active": True}, {"label": "编译"}, {"label": "打包"}, {"label": "部署"}
        ], "info"),
        ("running", "正在编译 C++ 核心模块...", 48, [
            {"label": "分析", "done": True}, {"label": "编译", "active": True}, {"label": "打包"}, {"label": "部署"}
        ], "none"),
        ("running", "正在打包 Docker 镜像并推送...", 82, [
            {"label": "分析", "done": True}, {"label": "编译", "done": True}, {"label": "打包", "active": True}, {"label": "部署"}
        ], "none"),
        ("success", "全部 4 个阶段执行完成！", 100, [
            {"label": "分析", "done": True}, {"label": "编译", "done": True}, {"label": "打包", "done": True}, {"label": "部署", "done": True}
        ], "success"),
    ]
    for st, text, pct, steps, snd in stages:
        print(f"  - 推送进度: {pct}% ({text})")
        req_json(host, "/api/status", {
            "state": st,
            "agent": "Deploy Bot",
            "title": "自动化构建与发布流水线",
            "subtitle": f"当前进度 {pct}%",
            "layout": "progress",
            "percent": pct,
            "step_text": text,
            "steps": steps,
            "sound": snd,
        }, timeout=10)
        time.sleep(1.3)
    print("  [完成] 进度展示结束，可点击屏幕右侧 [已知晓] 返回待机，或 3 秒后自动收起")
    time.sleep(3.0)
    req_json(host, "/api/dismiss", {}, timeout=10)


def check_connection(host: str) -> dict:
    st = req_json(host, "/api/state", timeout=5)
    print("终端在线状态:")
    print(json.dumps(st, ensure_ascii=False, indent=2))
    return st


def main() -> None:
    parser = argparse.ArgumentParser(description="Interactive test script for AI Ask-Me Terminal")
    parser.add_argument("--host", default=DEFAULT_HOST, help=f"ESP32 IP (default: {DEFAULT_HOST})")
    parser.add_argument("--all", action="store_true", help="Run all interactive tests sequentially")
    parser.add_argument("--check", action="store_true", help="Only check connectivity and /api/state")
    args = parser.parse_args()

    try:
        check_connection(args.host)
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        print(f"[错误] 无法连接到终端 http://{args.host}: {exc}", file=sys.stderr)
        sys.exit(1)

    if args.check:
        return

    if args.all:
        test_sound(args.host)
        test_text_ask(args.host)
        test_kv_ask(args.host)
        test_choice_ask(args.host)
        test_number_ask(args.host)
        test_progress_flow(args.host)
        print("\n全部 6 项测试已完成！")
        return

    menu = {
        "1": ("音效与自定义旋律展示 (/api/sound)", test_sound),
        "2": ("富文本确认 + 3 个右侧按钮 (layout=text)", test_text_ask),
        "3": ("KV 键值对审查 + 2 个按钮 (layout=kv)", test_kv_ask),
        "4": ("多选胶囊交互 (layout=choice)", test_choice_ask),
        "5": ("数值微调器交互 (layout=number)", test_number_ask),
        "6": ("AI 多阶段进度动态推进 (/api/status)", test_progress_flow),
    }

    while True:
        print("\n=== AI Ask-Me 提示终端实机测试菜单 ===")
        for k, (desc, _) in menu.items():
            print(f"  {k}. {desc}")
        print("  a. 按顺序运行全部 6 项测试")
        print("  d. 收起当前屏幕回到待机 (/api/dismiss)")
        print("  q. 退出")

        choice = input("\n请选择测试编号: ").strip().lower()
        if choice == "q":
            break
        elif choice == "d":
            print_result("收起屏幕", req_json(args.host, "/api/dismiss", {}, timeout=10))
        elif choice == "a":
            for k in ("1", "2", "3", "4", "5", "6"):
                menu[k][1](args.host)
        elif choice in menu:
            menu[choice][1](args.host)
        else:
            print("无效输入，请重试。")


if __name__ == "__main__":
    main()
