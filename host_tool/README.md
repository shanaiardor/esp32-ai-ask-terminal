# AI Ask-Me 提示终端调用与接入说明

> **推荐做法（让 AI 自己配置自己）**：不需要运行任何一键脚本，直接让当前使用的 AI Agent 阅读根目录下的 [AGENT_SETUP_PROMPTS.md](../AGENT_SETUP_PROMPTS.md)，AI 会根据自己的宿主环境（Pi / Antigravity / Claude Code / Cursor / Windsurf / OpenCode / Cline / 通用 Agent）自行完成 MCP、Hook/扩展、Skill 与规则配置。

## Host 工具目录 (`host_tool/`)

- [ai_terminal_mcp.py](./ai_terminal_mcp.py)：标准 **MCP (stdio JSON-RPC) Server**（5 个工具：`ask_user_on_terminal`、`show_ai_status_on_terminal`、`play_terminal_sound`、`dismiss_terminal_prompt`、`approve_tool_permission`）。
- [pi_extension/ai-ask-terminal.ts](./pi_extension/ai-ask-terminal.ts)：**Pi Coding Agent (`pi.dev`)** 原生 TypeScript 扩展（包含 `pi.on("tool_call")` 权限拦截、4 个原生工具与 `/terminal-mode`）。
- [permission_hook.py](./permission_hook.py)：兼容 **Antigravity / Claude Code `PreToolUse`** 的底层权限拦截钩子脚本（`smart` / `strict` 模式，设备离线自动回退桌面确认）。
- [guarded_exec.py](./guarded_exec.py)：面向无 MCP/Hook 的通用 Agent 的受控命令执行包装器。
- [skill/ai-ask-terminal/SKILL.md](./skill/ai-ask-terminal/SKILL.md)：跨平台标准 `SKILL.md` 说明。
- [ask_cli.py](./ask_cli.py) & [test_interactive.py](./test_interactive.py)：命令行测试与交互式实机菜单测试工具。
- [install_agent.py](./install_agent.py)：（可选）CLI 辅助安装脚本，如 AI 希望一步写入配置时也可按需调用。
