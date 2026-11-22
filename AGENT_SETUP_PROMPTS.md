# AI Agent 自主接入指南（AI Ask-Me 提示终端）

> **给人类用户的使用方法**：你不需要自己改任何配置文件。在 **任意 AI Agent**（Antigravity、Pi、Claude Code、Cursor、Windsurf、Codex、OpenCode、Cline 等）中，直接把下面这句话发给 AI 即可——**AI 自己会读懂这份文档，并按自己的机制完成所有配置：**
>
> > 请阅读本仓库的 `AGENT_SETUP_PROMPTS.md`，按其中针对你当前 Agent 架构的规范，帮我把你接入到桌面上的 ESP32 Ask-Me 提示终端（设备 IP 为 `<ESP32_IP>`），配置好工具调用、权限审批托管与交互规则，并在配置完成后发一条实机弹窗测试验证。

---

## 致阅读本指南的 AI Agent

你正在为用户把 **你自己（当前运行的 Agent 环境）** 接入一块桌面上的 **ESP32-S3-Touch-LCD-3.49 硬件交互提示终端**。  
不要盲目运行外部脚本猜测环境——请先确认设备连通性，再根据**你自己的宿主框架类型**，用你自己的文件读写工具精准修改对应配置。

### 本项目提供的现成工具资产（绝对路径参考）
- **标准 MCP Server（stdio JSON-RPC）**：`d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/ai_terminal_mcp.py`
  - 暴露 5 个工具：`ask_user_on_terminal`、`show_ai_status_on_terminal`、`play_terminal_sound`、`dismiss_terminal_prompt`、`approve_tool_permission`
- **通用 `PreToolUse` 权限拦截钩子脚本**：`d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/permission_hook.py`
  - 兼容 Antigravity / Claude Code 的 stdin/stdout JSON 契约，支持 `--mode smart`（只拦截高危/绕过沙箱命令）和 `--mode strict`（全部拦截），ESP32 离线时自动返回 `"decision": "ask"` 回退桌面弹窗。
- **Pi 原生 TypeScript 扩展**：`d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/pi_extension/ai-ask-terminal.ts`
- **通用 CLI & 受控 Shell 网关**：
  - `d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/ask_cli.py`
  - `d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/guarded_exec.py`
- **通用 Skill 文档**：`d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/skill/ai-ask-terminal/SKILL.md`

---

## 第一步：确认终端在线

先通过网络请求或命令行检查设备状态（将 `<ESP32_IP>` 替换为用户提供的 IP，默认 `192.168.3.152`）：
- `GET http://<ESP32_IP>/api/state`
- 返回 `{"ok": true, "ip": "...", "volume": ...}` 即表示终端在线就绪。

---

## 第二步：根据你的 Agent 架构自主完成配置

请识别你当前所处的 Agent，**只配置你自己对应的部分**（已有配置文件请先读取后合并，**严禁覆盖用户原有的其他 MCP/Hook/规则**）：

---

### 1. 如果你是 **Pi Coding Agent (`pi.dev`)**

Pi 采用轻量核心 + TypeScript 扩展架构，**优先使用原生 Extension，不需要额外折腾 MCP 适配器**：
1. **安装原生扩展**：
   - 将 `host_tool/pi_extension/ai-ask-terminal.ts` 复制到全局扩展目录 `~/.pi/agent/extensions/ai-ask-terminal.ts`（或项目级 `.pi/extensions/ai-ask-terminal.ts`），把其中的默认 IP 改成实际 `<ESP32_IP>`。
   - 该扩展通过 Pi 原生 API 做了两件事：
     - 用 `pi.on("tool_call", ...)` 拦截 `bash`/`write`/`edit` 工具调用：向 `POST http://<ESP32_IP>/api/ask` 发 `kv` 审批卡，用户拒绝则返回 `{ block: true, reason: "..." }` 直接阻断工具执行；用户点“转电脑确认”或设备离线时回退 `ctx.ui.confirm()`。
     - 用 `pi.registerTool(...)` 直接注册 `ask_user_on_terminal`、`show_ai_status_on_terminal`、`play_terminal_sound`、`dismiss_terminal_prompt` 四个受限 DSL 工具，并注册 `/terminal-mode smart|strict|off` 斜杠命令。
2. **安装 Skill 与 Rule**：
   - 将 `host_tool/skill/ai-ask-terminal/SKILL.md` 复制到 `~/.pi/agent/skills/ai-ask-terminal/SKILL.md`。
   - 在 `~/.pi/agent/AGENTS.md`（或项目 `AGENTS.md`）追加本文档末尾的 **【通用行为规则片段】**。
3. **生效方式**：提示用户在 Pi 中输入 `/reload` 即可热加载生效。

---

### 2. 如果你是 **Antigravity**

1. **挂载 MCP Server**：
   - 读取并合并编辑全局 `~/.gemini/config/mcp_config.json`，在 `"mcpServers"` 中加入：
     ```json
     "ai-ask-terminal": {
       "command": "python",
       "args": ["d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/ai_terminal_mcp.py"],
       "env": {
         "AI_TERMINAL_HOST": "<ESP32_IP>",
         "PYTHONIOENCODING": "utf-8"
       }
     }
     ```
2. **接管底层工具权限审批（`PreToolUse` Hook）**：
   - 读取并合并 `~/.gemini/config/hooks.json`（或当前项目 `.agents/hooks.json`），增加：
     ```json
     "esp32-permission-gate": {
       "enabled": true,
       "PreToolUse": [
         {
           "matcher": "run_command",
           "hooks": [
             {
               "type": "command",
               "command": "python d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/permission_hook.py --host <ESP32_IP> --mode smart",
               "timeout": 70
             }
           ]
         }
       ]
     }
     ```
   - 说明：Antigravity 会在执行 `run_command` 前向该脚本 `stdin` 传 `{ toolCall: { name, args }, cwd }`，脚本会弹窗到 ESP32 屏并往 `stdout` 返回 `{"decision": "allow" | "deny" | "ask"}`。
3. **写入行为规则**：
   - 在 `~/.gemini/config/GEMINI.md`（或项目 `AGENTS.md`）末尾追加 **【通用行为规则片段】**。

---

### 3. 如果你是 **Claude Code**

1. **挂载 MCP Server**：
   - 读取并合并 `~/.claude.json`（或项目 `.mcp.json`）中的 `"mcpServers"`，加入与上面相同的 `"ai-ask-terminal"` 配置。
2. **接管权限审批（两种方式任选其一，可同时提示用户）**：
   - **方式 A（`PreToolUse` Hook，无感自动生效）**：编辑 `~/.claude/settings.json`，在 `"hooks"."PreToolUse"` 数组中为 `"matcher": "Bash"` 添加命令 `python d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/permission_hook.py --host <ESP32_IP> --mode smart`。
   - **方式 B（原生全权委托参数）**：提示用户启动 Claude Code 时带上 `--permission-prompt-tool mcp__ai-ask-terminal__approve_tool_permission`，即可把 Claude Code 所有内置权限确认框 100% 委托给 `ai_terminal_mcp.py` 的 `approve_tool_permission` 工具。
3. **写入规则**：在 `~/.claude/CLAUDE.md` 追加 **【通用行为规则片段】**。

---

### 4. 如果你是 **Cursor / Windsurf / Cline / Roo Code / OpenCode / Codex CLI**

请先检查本地是否存在对应的配置文件，**只写入或合并 `"ai-ask-terminal"` 这一项**（保留原有字段）：

- **Cursor**：
  - MCP：`~/.cursor/mcp.json` → `mcpServers["ai-ask-terminal"]`（标准 stdio 格式）。
  - 规则：`~/.cursor/rules/ai-ask-terminal.mdc`（带 `alwaysApply: true` frontmatter）。
- **Windsurf**：
  - MCP：`~/.codeium/windsurf/mcp_config.json` → `mcpServers["ai-ask-terminal"]`。
- **OpenCode**：
  - MCP：`~/.config/opencode/opencode.json` → `mcp["ai-ask-terminal"]`，格式为：
    ```json
    {
      "type": "local",
      "command": ["python", "d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/ai_terminal_mcp.py"],
      "environment": { "AI_TERMINAL_HOST": "<ESP32_IP>", "PYTHONIOENCODING": "utf-8" },
      "enabled": true
    }
    ```
- **Codex CLI**：
  - MCP：`~/.codex/config.toml` → 追加 `[mcp_servers.ai_ask_terminal]`（`command = "python"`, `args = [...]`, `env = { AI_TERMINAL_HOST = "<ESP32_IP>", PYTHONIOENCODING = "utf-8" }`）。
  - 规则：`~/.codex/AGENTS.md` 追加 **【通用行为规则片段】**。
- **VS Code Cline / Roo Code**：
  - Cline：`%APPDATA%/Code/User/globalStorage/saoudrizwan.claude-dev/settings/cline_mcp_settings.json`
  - Roo Code：`%APPDATA%/Code/User/globalStorage/rooveterinaryinc.roo-cline/settings/mcp_settings.json`

---

### 5. 如果你是 **其他通用 Agent（如 Aider、自建框架、或完全不支持 MCP/Hook 的环境）**

即使你的环境既没有 MCP 也没有生命周期 Hook，你也可以通过以下 **零依赖通用协议** 接入：

1. **直接用 HTTP REST API 或 `ask_cli.py` 交互**：
   - 阻塞询问（等待屏幕触摸）：`POST http://<ESP32_IP>/api/ask`
   - 异步状态/进度推送：`POST http://<ESP32_IP>/api/status`
   - 仅播放提示音：`POST http://<ESP32_IP>/api/sound`
   - 收起回到待机：`POST http://<ESP32_IP>/api/dismiss`
   - **Windows 编码防坑提醒**：在 Windows CMD / PowerShell 中**禁止直接用 `curl -d '{...中文...}'` 传裸中文**（Windows 默认代码页会将其破坏成 `????`）；请优先使用 `ask_cli.py`、MCP / Pi 扩展工具，或将 JSON 中文转为 `\uXXXX` 转义序列 / 通过 UTF-8 文件 `--data-binary @payload.json` 发送。
2. **用 `guarded_exec.py` 自行实现高危命令审批**：
   - 将你的系统/指令规则设定为：执行高危 Shell 命令前，先经由 `python d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/guarded_exec.py --host <ESP32_IP> -- <原命令>` 包装执行；或者直接把 **受限 DSL 协议** 写入你自己的 Tool 定义中。

---

## 第三步：受限 DSL 协议规范（640 × 172 固定三栏排版）

无论通过 MCP、Pi 扩展、CLI 还是直接 HTTP `POST`，发往 `/api/ask` 或 `/api/status` 的 JSON **必须遵守以下受限结构**：

- **整体网格**：左栏 `Status Dock (128×160)` | 中栏 `Content Canvas (368×160)` | 右栏 `Action Dock (128×160)` + 底部全局倒计时条。**不允许传坐标/宽高/CSS。**
- **状态 `state`**：`"idle"` | `"thinking"` | `"asking"` | `"running"` | `"success"` | `"warning"` | `"error"`
- **中栏模板 `layout`（5 选 1）**：
  1. `"text"`：`body`（多行说明）+ 可选 `callout`（高亮提示条）
  2. `"kv"`：`kv` 数组（**最多 4 项** `{"k": "键", "v": "值", "highlight": true/false}`）
  3. `"progress"`：`percent` (`0~100`) + `step_text` + `steps` 数组（**最多 4 项** `{"label": "...", "done": bool, "active": bool}`）
  4. `"choice"`：`multi` (`true/false`) + `choices` 数组（**最多 6 项** `{"id": "...", "label": "...", "selected": bool}`），返回时附带 `selected_choices`
  5. `"number"`：`value`, `min`, `max`, `step`, `unit`，返回时附带 `number_value`
- **右栏按钮 `buttons`（1~4 个固定槽位）**：
  - 每项 `{"id": "...", "label": "...", "style": "primary|success|danger|warning|secondary", "is_default": bool}`
- **声音 `sound`**：`"none"` | `"info"` | `"ask"` | `"success"` | `"warning"` | `"error"` | `"urgent"`，或 `{"melody": "C5:80,E5:80,G5:120,C6:180", "repeat": 1}`

---

## 第四步：配置完成后的验收动作（请 AI 自主执行）

完成上述配置后，请你：
1. 向 `POST http://<ESP32_IP>/api/ask` 发送一条简短的 **接入验收弹窗**（例如 `layout: "kv"`，展示你刚才修改了哪个配置文件路径，并提供 `【确认生效】` / `【需调整】` 两个按钮），让用户在开发板触摸屏上亲手点一下完成测试；
2. 告诉用户修改了哪些文件，以及是否需要重载（例如 Pi 的 `/reload` 或新开对话加载 MCP）。

---

## 附录：可直接写入 Agent 记忆的【通用行为规则片段】

```markdown
## ESP32 硬件交互提示终端规则 (`ai-ask-terminal`)
- **交互与审批托管**：当需要向用户确认关键决策、高风险操作、多选方案或参数微调时，优先通过 `ask_user_on_terminal`（或 `ask_cli.py` / `POST /api/ask`）在桌面 ESP32 屏幕上弹窗并播放提示音，等待用户在物理屏幕上触摸点击后继续。
- **状态与进度同步**：执行耗时较长的任务（编译、烧录、测试、部署）时，通过 `show_ai_status_on_terminal`（或 `POST /api/status`）实时同步状态、进度条与完成提示音。
- **严守受限 DSL**：严格使用 `text`、`kv`（≤4行）、`progress`（≤4步）、`choice`（≤6项）、`number` 五种内置模板与 `1~4` 个固定槽位按钮，标题简明单行。
```
