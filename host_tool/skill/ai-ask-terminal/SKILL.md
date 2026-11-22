---
name: ai-ask-terminal
description: >-
  Interact with the user's desk ESP32-S3 640x172 Touch Terminal (sound chime,
  status/progress display, constrained DSL prompts, and hardware permission approvals)
  via MCP or CLI/HTTP fallback.
---

# ESP32 AI Ask-Me Terminal Skill

Use this skill whenever you need to notify the user audibly, display live task progress, request structured confirmation/choices/number adjustments, or obtain permission approval on the physical ESP32-S3 touch screen.

## Preferred Calling Path

1. **If `ai-ask-terminal` MCP / Pi Extension tools are loaded**:
   Call directly:
   - `ask_user_on_terminal` (blocking prompt, 1..4 buttons)
   - `show_ai_status_on_terminal` (non-blocking status/progress)
   - `play_terminal_sound` (sound chime / custom melody)
   - `dismiss_terminal_prompt` (clear screen back to idle)

2. **If MCP is unavailable (CLI / Generic Agent fallback)**:
   Use `host_tool/ask_cli.py`:
   ```powershell
   # Ask user with constrained KV or Text layout and block for touch click
   python d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/ask_cli.py --host $env:AI_TERMINAL_HOST ask `
     --state warning --title "是否执行变更？" --layout kv `
     --kv "*操作=部署固件,目标=COM5" `
     --buttons "allow:允许:success,deny:拒绝:danger:default" --sound ask

   # Push non-blocking progress
   python d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/ask_cli.py --host $env:AI_TERMINAL_HOST status `
     --state running --title "正在构建项目" --percent 60 --sound info
   ```

3. **Guarded Shell Execution (for Agents without native Hook support)**:
   Prefix risky shell commands with `guarded_exec.py` so the command pauses for physical screen approval before running:
   ```powershell
   python d:/ESP32/ESP32_lcd_349/workspace/ai_ask_terminal/host_tool/guarded_exec.py -- <command>
   ```

## Constrained DSL Rules

- **Screen Size**: Fixed 640×172 3-zone grid (Left Status Dock | Center Content | Right 1..4 Buttons).
- **Allowed `state`**: `idle`, `thinking`, `asking`, `running`, `success`, `warning`, `error`.
- **Allowed `layout`**: `text`, `kv` (max 4 items), `progress` (0..100 + max 4 steps), `choice` (max 6 chips), `number` (`value`/`min`/`max`/`step`/`unit`).
- **Allowed `buttons`**: 1..4 buttons; `style` must be one of `primary`, `success`, `danger`, `warning`, `secondary`.
- **Allowed `sound`**: `none`, `info`, `ask`, `success`, `warning`, `error`, `urgent`, or custom note sequence `"C5:80,E5:80,G5:150"`.
