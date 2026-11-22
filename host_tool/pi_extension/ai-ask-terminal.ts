/**
 * Pi Coding Agent Extension for ESP32 AI Ask-Me Terminal
 *
 * Placement:
 *   Global : ~/.pi/agent/extensions/ai-ask-terminal.ts
 *   Project: .pi/extensions/ai-ask-terminal.ts
 *
 * Features:
 *   1. Intercepts `bash` / `write` / `edit` tool calls via `pi.on("tool_call")`
 *      and prompts on the ESP32 640x172 touch screen with warning sound.
 *   2. Registers constrained-DSL tools (`ask_user_on_terminal`,
 *      `show_ai_status_on_terminal`, `play_terminal_sound`, `dismiss_terminal_prompt`).
 *   3. Provides `/terminal-mode <smart|strict|off>` slash command in Pi TUI.
 */

import type { ExtensionAPI } from "@mariozechner/pi-coding-agent";
import { Type } from "@sinclair/typebox";

const DEFAULT_HOST = process.env.AI_TERMINAL_HOST || "192.168.3.152";

type GateMode = "smart" | "strict" | "off";
let gateMode: GateMode = (process.env.AI_TERMINAL_HOOK_MODE as GateMode) || "smart";

const RISKY_PATTERNS = [
  "rm ",
  "rm\t",
  "rmdir",
  "del ",
  "erase ",
  "remove-item",
  "format ",
  "git push",
  "git reset",
  "git clean",
  "git checkout --",
  "pip install",
  "pip uninstall",
  "npm install",
  "npm i ",
  "pnpm add",
  "yarn add",
  "idf.py flash",
  "idf.py erase",
  "esptool",
  "curl ",
  "wget ",
  "sudo ",
  "chmod ",
  "chown ",
  "kill ",
];

function toAsciiSafeJson(obj: unknown): string {
  return JSON.stringify(obj).replace(/[\u007f-\uffff]/g, (ch) => {
    return "\\u" + ("0000" + ch.charCodeAt(0).toString(16)).slice(-4);
  });
}

async function postTerminal(path: string, payload: unknown, timeoutMs = 70000): Promise<any> {
  const controller = new AbortController();
  const timer = setTimeout(() => controller.abort(), timeoutMs);
  try {
    const resp = await fetch(`http://${DEFAULT_HOST}${path}`, {
      method: "POST",
      headers: { "Content-Type": "application/json; charset=utf-8" },
      body: toAsciiSafeJson(payload),
      signal: controller.signal,
    });
    return await resp.json();
  } finally {
    clearTimeout(timer);
  }
}

export default function aiAskTerminalExtension(pi: ExtensionAPI) {
  // 1. Slash command to toggle hardware permission gate mode dynamically
  pi.registerCommand("terminal-mode", {
    description: "Switch ESP32 terminal permission gate mode: smart | strict | off",
    handler: async (args, ctx) => {
      const next = (args || "").trim().toLowerCase();
      if (next === "smart" || next === "strict" || next === "off") {
        gateMode = next;
        ctx.ui.notify(`ESP32 terminal gate mode -> ${gateMode}`, "info");
      } else {
        ctx.ui.notify(`Current gate mode: ${gateMode} (usage: /terminal-mode smart|strict|off)`, "info");
      }
    },
  });

  // 2. Pre-execution permission gate via Pi's native tool_call event
  pi.on("tool_call", async (event, ctx) => {
    if (gateMode === "off") return;

    // Never gate our own terminal tools
    if (event.toolName.endsWith("_on_terminal") || event.toolName.includes("terminal_")) {
      return;
    }

    const input = (event.input ?? {}) as Record<string, unknown>;
    const cmd = String(input.command ?? "");
    const targetPath = String(input.path ?? input.file_path ?? "");
    const cmdLower = cmd.toLowerCase();

    if (event.toolName === "bash") {
      if (cmd.includes("ask_cli.py") || cmd.includes("ai_terminal_mcp.py")) return;
      const isRisky = RISKY_PATTERNS.some((p) => cmdLower.includes(p));
      if (gateMode === "smart" && !isRisky) return;
    } else if (event.toolName === "write" || event.toolName === "edit") {
      if (gateMode === "smart") return;
    } else {
      return;
    }

    const kv = [
      { k: "工具", v: event.toolName, highlight: true },
      { k: "参数", v: (cmd || targetPath || JSON.stringify(input)).slice(0, 62) },
      { k: "模式", v: `Pi ${gateMode.toUpperCase()} 拦截` },
    ];

    try {
      const res = await postTerminal("/api/ask", {
        wait: true,
        state: "warning",
        agent: "Pi Agent",
        title: `权限审批：${event.toolName}`,
        subtitle: "Pi tool_call 硬件安全网关",
        layout: "kv",
        kv,
        buttons: [
          { id: "allow", label: "允许执行", style: "success" },
          { id: "ask_ui", label: "转电脑确认", style: "primary" },
          { id: "deny", label: "拒绝拦截", style: "danger", is_default: true },
        ],
        timeout_sec: 60,
        sound: "warning",
      });

      if (res.action_id === "allow") {
        return;
      }
      if (res.action_id === "ask_ui") {
        // Fail closed: no TUI to confirm on => block instead of silently allowing.
        if (!ctx?.hasUI) {
          return { block: true, reason: "ESP32 asked to confirm on desktop, but no UI is available" };
        }
        const ok = await ctx.ui.confirm("ESP32 转交确认", `允许执行 ${event.toolName}: ${cmd || targetPath}?`);
        if (ok) return;
        return { block: true, reason: "Denied by user in desktop confirmation" };
      }
      return {
        block: true,
        reason: `Blocked by user on ESP32 Ask-Me Terminal (${res.action_id || res.status || "denied"})`,
      };
    } catch {
      // If ESP32 is offline, fallback gracefully to Pi TUI confirm for risky commands
      if (ctx?.hasUI) {
        const ok = await ctx.ui.confirm(
          "ESP32 离线回退确认",
          `允许执行 ${event.toolName}: ${(cmd || targetPath).slice(0, 120)}?`
        );
        if (ok) return;
        return { block: true, reason: "Denied by user in fallback confirmation" };
      }
      // Fail closed: terminal unreachable and no desktop UI => block.
      return { block: true, reason: "ESP32 terminal unreachable and no desktop UI for confirmation" };
    }
  });

  // 3. Register native Pi tools (zero external MCP adapter required)
  pi.registerTool({
    name: "ask_user_on_terminal",
    label: "Ask User on ESP32 Terminal",
    description:
      "Display a constrained 3-zone DSL prompt on the ESP32 640x172 touch screen, play a chime, and wait for button click.",
    parameters: Type.Object({
      state: Type.Optional(Type.Union([
        Type.Literal("idle"),
        Type.Literal("thinking"),
        Type.Literal("asking"),
        Type.Literal("running"),
        Type.Literal("success"),
        Type.Literal("warning"),
        Type.Literal("error"),
      ])),
      title: Type.String({ description: "Concise single-line prompt title" }),
      subtitle: Type.Optional(Type.String()),
      layout: Type.Union([
        Type.Literal("text"),
        Type.Literal("kv"),
        Type.Literal("choice"),
        Type.Literal("number"),
        Type.Literal("progress"),
      ]),
      body: Type.Optional(Type.String()),
      callout: Type.Optional(Type.String()),
      kv: Type.Optional(Type.Array(Type.Object({
        k: Type.String(),
        v: Type.String(),
        highlight: Type.Optional(Type.Boolean()),
      }), { maxItems: 4 })),
      choices: Type.Optional(Type.Array(Type.Object({
        id: Type.String(),
        label: Type.String(),
        selected: Type.Optional(Type.Boolean()),
      }), { maxItems: 6 })),
      multi: Type.Optional(Type.Boolean()),
      value: Type.Optional(Type.Number()),
      min: Type.Optional(Type.Number()),
      max: Type.Optional(Type.Number()),
      step: Type.Optional(Type.Number()),
      unit: Type.Optional(Type.String()),
      percent: Type.Optional(Type.Number({ minimum: 0, maximum: 100 })),
      step_text: Type.Optional(Type.String()),
      steps: Type.Optional(Type.Array(Type.Object({
        label: Type.String(),
        done: Type.Optional(Type.Boolean()),
        active: Type.Optional(Type.Boolean()),
      }), { maxItems: 4 })),
      buttons: Type.Array(Type.Object({
        id: Type.String(),
        label: Type.String(),
        style: Type.Optional(Type.Union([
          Type.Literal("primary"),
          Type.Literal("success"),
          Type.Literal("danger"),
          Type.Literal("warning"),
          Type.Literal("secondary"),
        ])),
        is_default: Type.Optional(Type.Boolean()),
      }), { minItems: 1, maxItems: 4 }),
      timeout_sec: Type.Optional(Type.Number()),
      sound: Type.Optional(Type.String()),
    }),
    async execute(_toolCallId, params) {
      const toSec = Number(params.timeout_sec ?? 60);
      const res = await postTerminal(
        "/api/ask",
        { agent: "Pi Agent", state: "asking", sound: "ask", ...params, wait: true },
        Math.max(15000, (toSec + 10) * 1000)
      );
      return {
        content: [{ type: "text", text: JSON.stringify(res, null, 2) }],
        details: res,
      };
    },
  });

  pi.registerTool({
    name: "show_ai_status_on_terminal",
    label: "Show AI Status on ESP32 Terminal",
    description: "Push non-blocking AI status, progress bar, and sound to the ESP32 terminal.",
    parameters: Type.Object({
      state: Type.Union([
        Type.Literal("idle"),
        Type.Literal("thinking"),
        Type.Literal("asking"),
        Type.Literal("running"),
        Type.Literal("success"),
        Type.Literal("warning"),
        Type.Literal("error"),
      ]),
      title: Type.String(),
      subtitle: Type.Optional(Type.String()),
      layout: Type.Optional(Type.Union([Type.Literal("text"), Type.Literal("kv"), Type.Literal("progress")])),
      body: Type.Optional(Type.String()),
      callout: Type.Optional(Type.String()),
      kv: Type.Optional(Type.Array(Type.Object({
        k: Type.String(),
        v: Type.String(),
        highlight: Type.Optional(Type.Boolean()),
      }), { maxItems: 4 })),
      percent: Type.Optional(Type.Number()),
      step_text: Type.Optional(Type.String()),
      steps: Type.Optional(Type.Array(Type.Object({
        label: Type.String(),
        done: Type.Optional(Type.Boolean()),
        active: Type.Optional(Type.Boolean()),
      }), { maxItems: 4 })),
      sound: Type.Optional(Type.String()),
    }),
    async execute(_toolCallId, params) {
      const res = await postTerminal("/api/status", { agent: "Pi Agent", ...params }, 10000);
      return {
        content: [{ type: "text", text: JSON.stringify(res, null, 2) }],
        details: res,
      };
    },
  });

  pi.registerTool({
    name: "play_terminal_sound",
    label: "Play Sound on ESP32 Terminal",
    description: "Play a preset chime or custom note melody on the ESP32 speaker.",
    parameters: Type.Object({
      preset: Type.Optional(Type.String()),
      melody: Type.Optional(Type.String()),
      repeat: Type.Optional(Type.Number()),
      volume: Type.Optional(Type.Number()),
      mute: Type.Optional(Type.Boolean()),
    }),
    async execute(_toolCallId, params) {
      const res = await postTerminal("/api/sound", params, 10000);
      return {
        content: [{ type: "text", text: JSON.stringify(res, null, 2) }],
        details: res,
      };
    },
  });

  pi.registerTool({
    name: "dismiss_terminal_prompt",
    label: "Dismiss ESP32 Terminal Prompt",
    description: "Dismiss the active prompt on ESP32 terminal and return to idle dashboard.",
    parameters: Type.Object({}),
    async execute() {
      const res = await postTerminal("/api/dismiss", {}, 10000);
      return {
        content: [{ type: "text", text: JSON.stringify(res, null, 2) }],
        details: res,
      };
    },
  });
}
