#!/usr/bin/env python3
"""
One-click cross-agent configurator for ESP32 AI Ask-Me Terminal.

Safely installs MCP server, native Extensions, PreToolUse Hooks, Skills, and Rules
for popular coding agents and generic frameworks:
  - antigravity : ~/.gemini/config/{mcp_config.json, hooks.json, skills/, GEMINI.md}
  - pi          : ~/.pi/agent/{extensions/ai-ask-terminal.ts, skills/ai-ask-terminal/SKILL.md, AGENTS.md}
  - claude      : ~/.claude.json (+ ~/.claude/{settings.json, skills/, CLAUDE.md})
  - cursor      : ~/.cursor/{mcp.json, rules/ai-ask-terminal.mdc}
  - windsurf    : ~/.codeium/windsurf/mcp_config.json
  - codex       : ~/.codex/{config.toml, AGENTS.md}
  - cline       : VS Code globalStorage saoudrizwan.claude-dev/settings/cline_mcp_settings.json
  - roo         : VS Code globalStorage rooveterinaryinc.roo-cline/settings/mcp_settings.json
  - opencode    : ~/.config/opencode/opencode.json
  - generic     : Installs cross-agent SKILL.md & prints CLI/HTTP/GuardedExec snippets
"""

import argparse
import json
import os
import sys
from pathlib import Path

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

HOST_TOOL_DIR = Path(__file__).resolve().parent
MCP_SCRIPT = HOST_TOOL_DIR.joinpath("ai_terminal_mcp.py").as_posix()
HOOK_SCRIPT = HOST_TOOL_DIR.joinpath("permission_hook.py").as_posix()
GUARD_SCRIPT = HOST_TOOL_DIR.joinpath("guarded_exec.py").as_posix()
CLI_SCRIPT = HOST_TOOL_DIR.joinpath("ask_cli.py").as_posix()
PI_EXT_SRC = HOST_TOOL_DIR / "pi_extension" / "ai-ask-terminal.ts"
SKILL_SRC = HOST_TOOL_DIR / "skill" / "ai-ask-terminal" / "SKILL.md"

RULE_MARKER = "<!-- AI_ASK_TERMINAL_RULE -->"


def build_rule_text(host: str, use_cli_fallback: bool = False) -> str:
    extra = ""
    if use_cli_fallback:
        extra = (
            f"\n- **CLI / 受控命令兜底**：若当前会话未加载 MCP 工具，可通过 `python {CLI_SCRIPT} --host {host}` "
            f"发消息或提问，高危命令可通过 `python {GUARD_SCRIPT} --host {host} -- <cmd>` 在硬件屏审批后执行。"
        )
    return f"""{RULE_MARKER}
## ESP32 硬件交互提示终端规则 (`ai-ask-terminal`)
- **交互询问托管**：当需要向用户确认重要决策、多选方案、参数微调或审批高风险操作时，优先调用 `ask_user_on_terminal` 在桌面 ESP32 (`{host}`) 屏幕上弹窗并播放提示音，等待用户触摸点击后继续。
- **状态与进度同步**：执行耗时超过 5 秒的任务（编译、烧录、测试、部署）时，调用 `show_ai_status_on_terminal` 同步当前阶段与进度，完成或出错时同步对应状态与音效。
- **遵循受限 DSL**：严格使用 `text`、`kv`、`progress`、`choice`、`number` 五种内置模板与 `1~4` 个固定槽位按钮，标题保持简明单行。{extra}
{RULE_MARKER}
"""


def load_json(path: Path) -> dict:
    if not path.exists():
        return {}
    with path.open("r", encoding="utf-8") as f:
        txt = f.read().strip()
        return json.loads(txt) if txt else {}


def save_json(path: Path, data: dict, dry_run: bool) -> None:
    content = json.dumps(data, ensure_ascii=False, indent=2) + "\n"
    if dry_run:
        print(f"[DRY-RUN] Would write {path}:\n{content}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print(f"[OK] Updated {path}")


def write_text_file(path: Path, text: str, dry_run: bool) -> None:
    if dry_run:
        print(f"[DRY-RUN] Would write {path} ({len(text)} chars)")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8", newline="\n")
    print(f"[OK] Wrote {path}")


def append_rule_file(path: Path, rule_text: str, dry_run: bool, with_frontmatter: bool = False) -> None:
    existing = path.read_text(encoding="utf-8") if path.exists() else ""
    if RULE_MARKER in existing:
        print(f"[SKIP] Rule already present in {path}")
        return
    block = rule_text
    if with_frontmatter and not existing:
        block = "---\ndescription: Prefer ESP32 AI Ask-Me Terminal for confirmations and status updates\nalwaysApply: true\n---\n\n" + rule_text
    new_text = (existing.rstrip() + "\n\n" + block) if existing.strip() else block
    if dry_run:
        print(f"[DRY-RUN] Would append rule to {path}")
        return
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(new_text, encoding="utf-8")
    print(f"[OK] Added rule to {path}")


def install_skill(skill_dir: Path, host: str, dry_run: bool) -> None:
    txt = SKILL_SRC.read_text(encoding="utf-8").replace("$env:AI_TERMINAL_HOST", host)
    write_text_file(skill_dir / "SKILL.md", txt, dry_run)


def mcp_entry(host: str) -> dict:
    return {
        "command": "python",
        "args": [MCP_SCRIPT],
        "env": {
            "AI_TERMINAL_HOST": host,
            "PYTHONIOENCODING": "utf-8",
        },
    }


def configure_antigravity(home: Path, host: str, enable_hook: bool, hook_mode: str, enable_rule: bool, dry_run: bool) -> None:
    cfg_dir = home / ".gemini" / "config"
    mcp_path = cfg_dir / "mcp_config.json"
    data = load_json(mcp_path)
    data.setdefault("mcpServers", {})["ai-ask-terminal"] = mcp_entry(host)
    save_json(mcp_path, data, dry_run)
    install_skill(cfg_dir / "skills" / "ai-ask-terminal", host, dry_run)

    if enable_hook:
        hooks_path = cfg_dir / "hooks.json"
        hdata = load_json(hooks_path)
        hdata["esp32-permission-gate"] = {
            "enabled": True,
            "PreToolUse": [
                {
                    "matcher": "run_command",
                    "hooks": [
                        {
                            "type": "command",
                            "command": f"python {HOOK_SCRIPT} --host {host} --mode {hook_mode}",
                            "timeout": 70,
                        }
                    ],
                }
            ],
        }
        save_json(hooks_path, hdata, dry_run)

    if enable_rule:
        append_rule_file(cfg_dir / "GEMINI.md", build_rule_text(host), dry_run)


def configure_pi(home: Path, host: str, enable_hook: bool, hook_mode: str, enable_rule: bool, dry_run: bool) -> None:
    pi_agent_dir = home / ".pi" / "agent"
    ext_dst = pi_agent_dir / "extensions" / "ai-ask-terminal.ts"
    ext_txt = PI_EXT_SRC.read_text(encoding="utf-8")
    ext_txt = ext_txt.replace('"192.168.3.152"', f'"{host}"')
    ext_txt = ext_txt.replace('|| "smart"', f'|| "{hook_mode if enable_hook else "off"}"')
    write_text_file(ext_dst, ext_txt, dry_run)

    install_skill(pi_agent_dir / "skills" / "ai-ask-terminal", host, dry_run)
    if enable_rule:
        append_rule_file(pi_agent_dir / "AGENTS.md", build_rule_text(host, use_cli_fallback=True), dry_run)


def configure_claude(home: Path, host: str, enable_hook: bool, hook_mode: str, enable_rule: bool, dry_run: bool) -> None:
    claude_json = home / ".claude.json"
    data = load_json(claude_json)
    data.setdefault("mcpServers", {})["ai-ask-terminal"] = mcp_entry(host)
    save_json(claude_json, data, dry_run)
    install_skill(home / ".claude" / "skills" / "ai-ask-terminal", host, dry_run)

    if enable_hook:
        settings_path = home / ".claude" / "settings.json"
        sdata = load_json(settings_path)
        hooks = sdata.setdefault("hooks", {})
        pre = [x for x in hooks.get("PreToolUse", []) if "permission_hook.py" not in json.dumps(x)]
        pre.append({
            "matcher": "Bash",
            "hooks": [
                {
                    "type": "command",
                    "command": f"python {HOOK_SCRIPT} --host {host} --mode {hook_mode}",
                    "timeout": 70,
                }
            ],
        })
        hooks["PreToolUse"] = pre
        save_json(settings_path, sdata, dry_run)

    if enable_rule:
        append_rule_file(home / ".claude" / "CLAUDE.md", build_rule_text(host), dry_run)


def configure_cursor(home: Path, host: str, enable_rule: bool, dry_run: bool) -> None:
    mcp_path = home / ".cursor" / "mcp.json"
    data = load_json(mcp_path)
    data.setdefault("mcpServers", {})["ai-ask-terminal"] = mcp_entry(host)
    save_json(mcp_path, data, dry_run)

    if enable_rule:
        append_rule_file(home / ".cursor" / "rules" / "ai-ask-terminal.mdc", build_rule_text(host), dry_run, with_frontmatter=True)


def configure_windsurf(home: Path, host: str, dry_run: bool) -> None:
    mcp_path = home / ".codeium" / "windsurf" / "mcp_config.json"
    data = load_json(mcp_path)
    data.setdefault("mcpServers", {})["ai-ask-terminal"] = mcp_entry(host)
    save_json(mcp_path, data, dry_run)


def configure_codex(home: Path, host: str, enable_rule: bool, dry_run: bool) -> None:
    toml_path = home / ".codex" / "config.toml"
    existing = toml_path.read_text(encoding="utf-8") if toml_path.exists() else ""
    if "[mcp_servers.ai_ask_terminal]" in existing:
        print(f"[SKIP] [mcp_servers.ai_ask_terminal] already in {toml_path}")
    else:
        block = (
            "\n[mcp_servers.ai_ask_terminal]\n"
            'command = "python"\n'
            f'args = ["{MCP_SCRIPT}"]\n'
            f'env = {{ AI_TERMINAL_HOST = "{host}", PYTHONIOENCODING = "utf-8" }}\n'
        )
        if dry_run:
            print(f"[DRY-RUN] Would append to {toml_path}:\n{block}")
        else:
            toml_path.parent.mkdir(parents=True, exist_ok=True)
            toml_path.write_text(existing + block, encoding="utf-8")
            print(f"[OK] Updated {toml_path}")
    if enable_rule:
        append_rule_file(home / ".codex" / "AGENTS.md", build_rule_text(host, use_cli_fallback=True), dry_run)


def _vscode_global_storage_base(home: Path) -> Path:
    if sys.platform == "win32":
        appdata = os.environ.get("APPDATA")
        return (Path(appdata) if appdata else (home / "AppData" / "Roaming")) / "Code" / "User" / "globalStorage"
    if sys.platform == "darwin":
        return home / "Library" / "Application Support" / "Code" / "User" / "globalStorage"
    return home / ".config" / "Code" / "User" / "globalStorage"


def configure_cline_or_roo(home: Path, host: str, kind: str, dry_run: bool) -> None:
    base = _vscode_global_storage_base(home)
    if kind == "cline":
        mcp_path = base / "saoudrizwan.claude-dev" / "settings" / "cline_mcp_settings.json"
    else:
        mcp_path = base / "rooveterinaryinc.roo-cline" / "settings" / "mcp_settings.json"
    data = load_json(mcp_path)
    data.setdefault("mcpServers", {})["ai-ask-terminal"] = mcp_entry(host)
    save_json(mcp_path, data, dry_run)


def configure_opencode(home: Path, host: str, dry_run: bool) -> None:
    cfg_path = home / ".config" / "opencode" / "opencode.json"
    data = load_json(cfg_path)
    mcp = data.setdefault("mcp", {})
    mcp["ai-ask-terminal"] = {
        "type": "local",
        "command": ["python", MCP_SCRIPT],
        "environment": {
            "AI_TERMINAL_HOST": host,
            "PYTHONIOENCODING": "utf-8",
        },
        "enabled": True,
    }
    save_json(cfg_path, data, dry_run)


def configure_generic(home: Path, host: str, dry_run: bool) -> None:
    # Install universal ~/.agents/skills/ai-ask-terminal/SKILL.md
    install_skill(home / ".agents" / "skills" / "ai-ask-terminal", host, dry_run)
    print("\n--- Generic Fallback Summary (Aider / LangChain / Custom Agents) ---")
    print(f"  HTTP Endpoint : http://{host}/api/ask (POST, blocking) & /api/status (POST, async)")
    print(f"  CLI Tool      : python {CLI_SCRIPT} --host {host} ask --title ... --layout ... --buttons ...")
    print(f"  Guarded Shell : python {GUARD_SCRIPT} --host {host} -- <command>")
    print(f"  MCP Stdio     : python {MCP_SCRIPT} --host {host}")


def main() -> None:
    all_agents = ["antigravity", "pi", "claude", "cursor", "windsurf", "codex", "cline", "roo", "opencode", "generic"]
    parser = argparse.ArgumentParser(description="Configure AI agents to use ESP32 AI Ask-Me Terminal")
    parser.add_argument(
        "--agent",
        choices=all_agents + ["all"],
        default="antigravity",
        help="Target AI agent (or 'all')",
    )
    parser.add_argument("--host", default=os.environ.get("AI_TERMINAL_HOST", "192.168.3.152"), help="ESP32 terminal IP")
    parser.add_argument("--enable-hook", action="store_true", help="Install permission gate hook/extension")
    parser.add_argument("--hook-mode", choices=["smart", "strict"], default="smart", help="Hook gating mode")
    parser.add_argument("--enable-rule", action="store_true", help="Install agent prompt rule to prefer hardware terminal")
    parser.add_argument("--dry-run", action="store_true", help="Preview changes without modifying files")
    args = parser.parse_args()

    home = Path.home()
    targets = all_agents if args.agent == "all" else [args.agent]

    for t in targets:
        if t == "antigravity":
            configure_antigravity(home, args.host, args.enable_hook, args.hook_mode, args.enable_rule, args.dry_run)
        elif t == "pi":
            configure_pi(home, args.host, args.enable_hook, args.hook_mode, args.enable_rule, args.dry_run)
        elif t == "claude":
            configure_claude(home, args.host, args.enable_hook, args.hook_mode, args.enable_rule, args.dry_run)
        elif t == "cursor":
            configure_cursor(home, args.host, args.enable_rule, args.dry_run)
        elif t == "windsurf":
            configure_windsurf(home, args.host, args.dry_run)
        elif t == "codex":
            configure_codex(home, args.host, args.enable_rule, args.dry_run)
        elif t in ("cline", "roo"):
            configure_cline_or_roo(home, args.host, t, args.dry_run)
        elif t == "opencode":
            configure_opencode(home, args.host, args.dry_run)
        elif t == "generic":
            configure_generic(home, args.host, args.dry_run)


if __name__ == "__main__":
    main()
