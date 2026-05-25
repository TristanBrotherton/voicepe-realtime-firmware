# Voice PE → voice-assistant direct streaming — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace HA voice_assistant pipeline with a direct WebSocket between Voice PE firmware and voice-assistant backed by OpenAI Realtime API. Cut end-to-end latency from multiple seconds to sub-second.

**Architecture:** Voice PE captures audio via XMOS DSP, sends to voice-assistant over WS. voice-assistant bridges to OpenAI Realtime (audio-in/audio-out, single pipe), forwards tool calls to existing HA MCP client. TTS audio streams back to the device. Music/timers/media_player on Voice PE are dropped — this is a voice-only appliance.

**Tech Stack:**
- Firmware: ESPHome custom C++ component, esp-idf WebSocket client
- voice-assistant: Node.js 24+, TypeScript, `ws`, OpenAI SDK, `vitest`, `pino`
- Audio codec: raw PCM16 both directions (Opus dropped — see spec
  "codec policy" section; LAN bandwidth is plentiful, ESP32 CPU isn't)
- Infra: docker-compose on Raspberry Pi 5

**Spec:** [docs/superpowers/specs/2026-05-25-voice-pe-direct-va-streaming-design.md](../specs/2026-05-25-voice-pe-direct-va-streaming-design.md)

---

## Repos touched

| Repo | Path under `~/Developer/home/` |
| --- | --- |
| Firmware | `home-assistant-voice-pe/` |
| voice-assistant | `voice-assistant/` |
| Infra | `home-infra/` |

Each task explicitly names which repo it's in. Cross-repo coordination happens at milestone boundaries (M2 and M4).

## File Structure

### voice-assistant (new)

| File | Responsibility |
| --- | --- |
| `src/realtime/wsServer.ts` | HTTP+WS server on port 3001, `/voice` upgrade, auth |
| `src/realtime/auth.ts` | Bearer token check, constant-time compare |
| `src/realtime/protocol.ts` | TypeScript types for JSON control messages |
| `src/realtime/realtimeBridge.ts` | One-session orchestrator: device WS ↔ OpenAI Realtime |
| `src/realtime/openaiRealtimeClient.ts` | Thin wrapper over OpenAI Realtime WS API |
| `src/realtime/toolAdapter.ts` | Convert HA MCP tool list → Realtime tool schema; route tool_calls |
| `src/realtime/audio/resample.ts` | Linear resampler 16↔24 kHz mono PCM16 |
| `src/realtime/audio/format.ts` | PCM16 helpers (base64 ↔ Buffer) |
| `src/realtime/metrics.ts` | Latency tracker (wake → first_audio_out etc.), pino logs |
| `src/realtime/index.ts` | `startRealtimeServer({ port, token, agent })` entrypoint |
| `src/cli/realtimeSmoke.ts` | CLI smoke client: feeds a WAV file, dumps response PCM |
| `tests/realtime/auth.test.ts` | Unit tests for token check |
| `tests/realtime/protocol.test.ts` | Schema validation |
| `tests/realtime/resample.test.ts` | Resampler correctness |
| `tests/realtime/toolAdapter.test.ts` | Tool schema conversion, tool_call routing |
| `tests/realtime/wsServer.test.ts` | Integration: WS upgrade, auth, audio echo (with mock OpenAI) |

### voice-assistant (modified)

| File | Change |
| --- | --- |
| `package.json` | Add `ws`, `@types/ws`. |
| `src/cli/unified.ts` | Boot `startRealtimeServer` when `REALTIME_ENABLED=1` |
| `src/config.ts` | Read `REALTIME_PORT`, `VA_DEVICE_TOKEN`, `OPENAI_REALTIME_MODEL` |

### Firmware (new) — M2 onward

| File | Responsibility |
| --- | --- |
| `esphome/components/va_client/__init__.py` | ESPHome codegen, yaml schema |
| `esphome/components/va_client/va_client.h` | Class declaration |
| `esphome/components/va_client/va_client.cpp` | WS client + audio queues |
| `esphome/components/va_client/audio_queue.h` | Lock-free ring buffer for playback |
| ~~`opus_codec.cpp`~~ | Dropped — raw PCM16 stays end-to-end |
| `home-assistant-voice.va-direct.yaml` | New top-level yaml — voice-only flavour |

### Firmware (modified) — M5

| File | Change |
| --- | --- |
| `home-assistant-voice.yaml` | Deleted (replaced by `va-direct` renamed) |
| `home-assistant-voice.va-direct.yaml` | Renamed to main |
| `README.md` | Note non-stock fork, no media_player |

### home-infra (modified) — M4

| File | Change |
| --- | --- |
| `docker-compose.yml` | Expose port 3001 on `voice-assistant` service |
| `.env.example` | Add `VA_DEVICE_TOKEN`, `REALTIME_ENABLED` |
| `CLAUDE.md` | Document new WS contract |

---

# M1 — voice-assistant skeleton + CLI smoke

**Goal:** voice-assistant serves `/voice` WS, bridges to OpenAI Realtime, routes tool calls via HA MCP. CLI client feeds a WAV, gets a PCM response back. **No firmware changes in this milestone.**

**Definition of done:**
- `npm test` passes.
- `npm run start` with `REALTIME_ENABLED=1` starts the WS server.
- `node src/cli/realtimeSmoke.ts ./sample.wav` connects, streams audio, prints latency, writes `out.pcm`.
- `ffplay -f s16le -ar 24000 -ac 1 out.pcm` plays a sane reply.

## Task M1.1: Add `ws` dependency

**Files:**
- Modify: `voice-assistant/package.json`

- [ ] **Step 1: Install ws + types**

```bash
cd voice-assistant
npm install ws
npm install -D @types/ws
```

- [ ] **Step 2: Verify lockfile updated**

Run: `git diff package.json package-lock.json | head -20`
Expected: `ws` and `@types/ws` appear in dependencies / devDependencies.

- [ ] **Step 3: Commit**

```bash
git add package.json package-lock.json
git commit -m "deps: add ws + @types/ws for realtime endpoint"
```

## Task M1.2: Define wire protocol types

**Files:**
- Create: `voice-assistant/src/realtime/protocol.ts`
- Create: `voice-assistant/tests/realtime/protocol.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
// tests/realtime/protocol.test.ts
import { describe, it, expect } from 'vitest';
import {
  parseDeviceMessage,
  encodeServerMessage,
  type ServerMessage,
} from '../../src/realtime/protocol.js';

describe('protocol', () => {
  it('parses a valid start message', () => {
    const msg = parseDeviceMessage('{"type":"start"}');
    expect(msg).toEqual({ type: 'start' });
  });

  it('parses an interrupt message', () => {
    const msg = parseDeviceMessage('{"type":"interrupt"}');
    expect(msg).toEqual({ type: 'interrupt' });
  });

  it('parses a ping message', () => {
    const msg = parseDeviceMessage('{"type":"ping"}');
    expect(msg).toEqual({ type: 'ping' });
  });

  it('rejects unknown message types', () => {
    expect(() => parseDeviceMessage('{"type":"nope"}')).toThrow();
  });

  it('rejects malformed JSON', () => {
    expect(() => parseDeviceMessage('not json')).toThrow();
  });

  it('encodes a phase message', () => {
    const out: ServerMessage = { type: 'phase', value: 'listening' };
    expect(encodeServerMessage(out)).toBe('{"type":"phase","value":"listening"}');
  });

  it('encodes an error message', () => {
    expect(encodeServerMessage({ type: 'error', message: 'boom' })).toBe(
      '{"type":"error","message":"boom"}',
    );
  });
});
```

- [ ] **Step 2: Run test, verify it fails**

Run: `cd voice-assistant && npx vitest run tests/realtime/protocol.test.ts`
Expected: FAIL — module not found.

- [ ] **Step 3: Implement protocol module**

```ts
// src/realtime/protocol.ts
import { z } from 'zod';

export const DeviceMessageSchema = z.discriminatedUnion('type', [
  z.object({ type: z.literal('start') }),
  z.object({ type: z.literal('interrupt') }),
  z.object({ type: z.literal('ping') }),
]);

export type DeviceMessage = z.infer<typeof DeviceMessageSchema>;

export type Phase = 'idle' | 'listening' | 'thinking' | 'replying';

export type ServerMessage =
  | { type: 'phase'; value: Phase }
  | { type: 'error'; message: string }
  | { type: 'pong' }
  | { type: 'hello'; audioOut: 'pcm' };

export function parseDeviceMessage(raw: string): DeviceMessage {
  const json = JSON.parse(raw);
  return DeviceMessageSchema.parse(json);
}

export function encodeServerMessage(msg: ServerMessage): string {
  return JSON.stringify(msg);
}
```

- [ ] **Step 4: Run test, verify pass**

Run: `npx vitest run tests/realtime/protocol.test.ts`
Expected: PASS, 7/7.

- [ ] **Step 5: Commit**

```bash
git add src/realtime/protocol.ts tests/realtime/protocol.test.ts
git commit -m "feat(realtime): wire protocol types and parser"
```

## Task M1.3: Bearer token auth

**Files:**
- Create: `voice-assistant/src/realtime/auth.ts`
- Create: `voice-assistant/tests/realtime/auth.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
// tests/realtime/auth.test.ts
import { describe, it, expect } from 'vitest';
import { verifyBearer } from '../../src/realtime/auth.js';

describe('verifyBearer', () => {
  it('accepts correct token', () => {
    expect(verifyBearer('Bearer abc123', 'abc123')).toBe(true);
  });

  it('rejects wrong token', () => {
    expect(verifyBearer('Bearer wrong', 'abc123')).toBe(false);
  });

  it('rejects missing header', () => {
    expect(verifyBearer(undefined, 'abc123')).toBe(false);
  });

  it('rejects non-Bearer scheme', () => {
    expect(verifyBearer('Basic abc123', 'abc123')).toBe(false);
  });

  it('rejects empty expected token', () => {
    expect(verifyBearer('Bearer abc123', '')).toBe(false);
  });
});
```

- [ ] **Step 2: Run, expect fail**

Run: `npx vitest run tests/realtime/auth.test.ts`
Expected: FAIL.

- [ ] **Step 3: Implement**

```ts
// src/realtime/auth.ts
import { timingSafeEqual } from 'node:crypto';

export function verifyBearer(header: string | undefined, expected: string): boolean {
  if (!expected || !header) return false;
  if (!header.startsWith('Bearer ')) return false;
  const provided = header.slice('Bearer '.length);
  const a = Buffer.from(provided);
  const b = Buffer.from(expected);
  if (a.length !== b.length) return false;
  return timingSafeEqual(a, b);
}
```

- [ ] **Step 4: Run, expect pass**

Run: `npx vitest run tests/realtime/auth.test.ts`
Expected: PASS, 5/5.

- [ ] **Step 5: Commit**

```bash
git add src/realtime/auth.ts tests/realtime/auth.test.ts
git commit -m "feat(realtime): bearer token auth with timing-safe compare"
```

## Task M1.4: PCM format helpers

**Files:**
- Create: `voice-assistant/src/realtime/audio/format.ts`
- Create: `voice-assistant/tests/realtime/format.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
// tests/realtime/format.test.ts
import { describe, it, expect } from 'vitest';
import { pcm16ToBase64, base64ToPcm16 } from '../../src/realtime/audio/format.js';

describe('pcm16 helpers', () => {
  it('round-trips a buffer', () => {
    const buf = Buffer.from([0x00, 0x01, 0xff, 0x7f, 0x80, 0x80]);
    const b64 = pcm16ToBase64(buf);
    expect(base64ToPcm16(b64).equals(buf)).toBe(true);
  });

  it('rejects odd-length pcm', () => {
    expect(() => pcm16ToBase64(Buffer.from([0x00, 0x01, 0x02]))).toThrow();
  });
});
```

- [ ] **Step 2: Run, expect fail**

Run: `npx vitest run tests/realtime/format.test.ts`

- [ ] **Step 3: Implement**

```ts
// src/realtime/audio/format.ts
export function pcm16ToBase64(pcm: Buffer): string {
  if (pcm.length % 2 !== 0) {
    throw new Error(`pcm16 buffer must have even length, got ${pcm.length}`);
  }
  return pcm.toString('base64');
}

export function base64ToPcm16(b64: string): Buffer {
  return Buffer.from(b64, 'base64');
}
```

- [ ] **Step 4: Run, expect pass**

- [ ] **Step 5: Commit**

```bash
git add src/realtime/audio/format.ts tests/realtime/format.test.ts
git commit -m "feat(realtime): pcm16 base64 helpers"
```

## Task M1.5: Linear resampler 16↔24 kHz

**Files:**
- Create: `voice-assistant/src/realtime/audio/resample.ts`
- Create: `voice-assistant/tests/realtime/resample.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
// tests/realtime/resample.test.ts
import { describe, it, expect } from 'vitest';
import { resamplePcm16 } from '../../src/realtime/audio/resample.js';

function makeTone(freq: number, sampleRate: number, durationMs: number): Buffer {
  const n = Math.round((sampleRate * durationMs) / 1000);
  const buf = Buffer.alloc(n * 2);
  for (let i = 0; i < n; i++) {
    const s = Math.round(Math.sin((2 * Math.PI * freq * i) / sampleRate) * 16000);
    buf.writeInt16LE(s, i * 2);
  }
  return buf;
}

describe('resamplePcm16', () => {
  it('upsamples 16k → 24k preserves duration ±1 sample', () => {
    const src = makeTone(440, 16000, 100); // 1600 samples
    const out = resamplePcm16(src, 16000, 24000);
    expect(out.length / 2).toBeGreaterThanOrEqual(2399);
    expect(out.length / 2).toBeLessThanOrEqual(2401);
  });

  it('downsamples 24k → 16k', () => {
    const src = makeTone(440, 24000, 100); // 2400 samples
    const out = resamplePcm16(src, 24000, 16000);
    expect(out.length / 2).toBeGreaterThanOrEqual(1599);
    expect(out.length / 2).toBeLessThanOrEqual(1601);
  });

  it('passthrough when rates equal', () => {
    const src = makeTone(440, 16000, 100);
    const out = resamplePcm16(src, 16000, 16000);
    expect(out.equals(src)).toBe(true);
  });
});
```

- [ ] **Step 2: Run, expect fail**

- [ ] **Step 3: Implement**

```ts
// src/realtime/audio/resample.ts
export function resamplePcm16(src: Buffer, fromRate: number, toRate: number): Buffer {
  if (fromRate === toRate) return Buffer.from(src);
  const srcSamples = src.length / 2;
  const ratio = toRate / fromRate;
  const dstSamples = Math.round(srcSamples * ratio);
  const dst = Buffer.alloc(dstSamples * 2);
  for (let i = 0; i < dstSamples; i++) {
    const srcPos = i / ratio;
    const i0 = Math.floor(srcPos);
    const i1 = Math.min(i0 + 1, srcSamples - 1);
    const frac = srcPos - i0;
    const s0 = src.readInt16LE(i0 * 2);
    const s1 = src.readInt16LE(i1 * 2);
    const s = Math.round(s0 * (1 - frac) + s1 * frac);
    dst.writeInt16LE(s, i * 2);
  }
  return dst;
}
```

- [ ] **Step 4: Run, expect pass**

- [ ] **Step 5: Commit**

```bash
git add src/realtime/audio/resample.ts tests/realtime/resample.test.ts
git commit -m "feat(realtime): linear resampler for pcm16"
```

## Task M1.6: Tool adapter (MCP → Realtime schema)

**Files:**
- Create: `voice-assistant/src/realtime/toolAdapter.ts`
- Create: `voice-assistant/tests/realtime/toolAdapter.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
// tests/realtime/toolAdapter.test.ts
import { describe, it, expect } from 'vitest';
import { mcpToolsToRealtime } from '../../src/realtime/toolAdapter.js';

describe('mcpToolsToRealtime', () => {
  it('converts MCP tool to Realtime tool definition', () => {
    const mcp = [
      {
        name: 'HassTurnOn',
        description: 'Turn on an entity',
        inputSchema: {
          type: 'object',
          properties: { entity_id: { type: 'string' } },
          required: ['entity_id'],
        },
      },
    ];
    const out = mcpToolsToRealtime(mcp);
    expect(out).toEqual([
      {
        type: 'function',
        name: 'HassTurnOn',
        description: 'Turn on an entity',
        parameters: {
          type: 'object',
          properties: { entity_id: { type: 'string' } },
          required: ['entity_id'],
        },
      },
    ]);
  });

  it('skips tools with missing schema', () => {
    const out = mcpToolsToRealtime([{ name: 'broken' }] as any);
    expect(out).toEqual([]);
  });
});
```

- [ ] **Step 2: Run, expect fail**

- [ ] **Step 3: Implement**

```ts
// src/realtime/toolAdapter.ts
export interface McpTool {
  name: string;
  description?: string;
  inputSchema?: Record<string, unknown>;
}

export interface RealtimeTool {
  type: 'function';
  name: string;
  description: string;
  parameters: Record<string, unknown>;
}

export function mcpToolsToRealtime(tools: McpTool[]): RealtimeTool[] {
  const out: RealtimeTool[] = [];
  for (const t of tools) {
    if (!t.inputSchema || !t.name) continue;
    out.push({
      type: 'function',
      name: t.name,
      description: t.description ?? '',
      parameters: t.inputSchema,
    });
  }
  return out;
}
```

- [ ] **Step 4: Run, expect pass**

- [ ] **Step 5: Commit**

```bash
git add src/realtime/toolAdapter.ts tests/realtime/toolAdapter.test.ts
git commit -m "feat(realtime): convert MCP tools to Realtime tool definitions"
```

## Task M1.7: OpenAI Realtime client wrapper

**Files:**
- Create: `voice-assistant/src/realtime/openaiRealtimeClient.ts`

> No unit test here — this is a thin WS wrapper. Behavior is covered by the integration test in M1.10 using a mock OpenAI server.

- [ ] **Step 1: Implement**

```ts
// src/realtime/openaiRealtimeClient.ts
import WebSocket from 'ws';
import { pino } from 'pino';
import type { RealtimeTool } from './toolAdapter.js';

const log = pino({ name: 'openai-realtime' });

export interface RealtimeClientOptions {
  apiKey: string;
  model: string;
  instructions: string;
  tools: RealtimeTool[];
  voice: string;
}

export type RealtimeEvent =
  | { type: 'session.created'; session: unknown }
  | { type: 'input_audio_buffer.speech_started' }
  | { type: 'input_audio_buffer.speech_stopped' }
  | { type: 'response.created'; response: { id: string } }
  | { type: 'response.audio.delta'; delta: string; response_id: string }
  | { type: 'response.audio.done'; response_id: string }
  | { type: 'response.done'; response: { id: string; output: unknown[] } }
  | {
      type: 'response.function_call_arguments.done';
      call_id: string;
      name: string;
      arguments: string;
    }
  | { type: 'error'; error: { message: string } }
  | { type: string; [k: string]: unknown };

export class OpenAiRealtimeClient {
  private ws: WebSocket | null = null;
  private listeners: ((ev: RealtimeEvent) => void)[] = [];

  constructor(private opts: RealtimeClientOptions) {}

  async connect(): Promise<void> {
    const url = `wss://api.openai.com/v1/realtime?model=${encodeURIComponent(this.opts.model)}`;
    this.ws = new WebSocket(url, {
      headers: {
        Authorization: `Bearer ${this.opts.apiKey}`,
        'OpenAI-Beta': 'realtime=v1',
      },
    });
    await new Promise<void>((resolve, reject) => {
      this.ws!.once('open', () => resolve());
      this.ws!.once('error', reject);
    });
    this.ws!.on('message', (data) => {
      try {
        const ev = JSON.parse(data.toString()) as RealtimeEvent;
        for (const l of this.listeners) l(ev);
      } catch (err) {
        log.warn({ err }, 'failed to parse realtime event');
      }
    });
    this.send({
      type: 'session.update',
      session: {
        modalities: ['audio', 'text'],
        instructions: this.opts.instructions,
        voice: this.opts.voice,
        input_audio_format: 'pcm16',
        output_audio_format: 'pcm16',
        turn_detection: { type: 'server_vad' },
        tools: this.opts.tools,
      },
    });
  }

  on(listener: (ev: RealtimeEvent) => void): void {
    this.listeners.push(listener);
  }

  send(msg: unknown): void {
    if (!this.ws || this.ws.readyState !== WebSocket.OPEN) {
      throw new Error('openai realtime ws not open');
    }
    this.ws.send(JSON.stringify(msg));
  }

  appendAudioPcm16Base64(b64: string): void {
    this.send({ type: 'input_audio_buffer.append', audio: b64 });
  }

  cancelResponse(): void {
    this.send({ type: 'response.cancel' });
    this.send({ type: 'input_audio_buffer.clear' });
  }

  submitToolResult(callId: string, output: string): void {
    this.send({
      type: 'conversation.item.create',
      item: {
        type: 'function_call_output',
        call_id: callId,
        output,
      },
    });
    this.send({ type: 'response.create' });
  }

  close(): void {
    this.ws?.close();
    this.ws = null;
  }
}
```

- [ ] **Step 2: Type-check**

Run: `npm run typecheck`
Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add src/realtime/openaiRealtimeClient.ts
git commit -m "feat(realtime): OpenAI Realtime WS client wrapper"
```

## Task M1.8: Latency metrics

**Files:**
- Create: `voice-assistant/src/realtime/metrics.ts`
- Create: `voice-assistant/tests/realtime/metrics.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
// tests/realtime/metrics.test.ts
import { describe, it, expect, vi } from 'vitest';
import { LatencyTracker } from '../../src/realtime/metrics.js';

describe('LatencyTracker', () => {
  it('reports deltas between markers', () => {
    let now = 1000;
    const t = new LatencyTracker(() => now);
    t.mark('start');
    now = 1100;
    t.mark('first_audio_in');
    now = 1500;
    t.mark('first_audio_out');
    const r = t.report();
    expect(r['start→first_audio_in']).toBe(100);
    expect(r['first_audio_in→first_audio_out']).toBe(400);
    expect(r['start→first_audio_out']).toBe(500);
  });
});
```

- [ ] **Step 2: Run, expect fail**

- [ ] **Step 3: Implement**

```ts
// src/realtime/metrics.ts
import { pino } from 'pino';

const log = pino({ name: 'realtime-metrics' });

export class LatencyTracker {
  private marks: Map<string, number> = new Map();
  private order: string[] = [];

  constructor(private now: () => number = () => Date.now()) {}

  mark(name: string): void {
    if (this.marks.has(name)) return;
    this.marks.set(name, this.now());
    this.order.push(name);
  }

  report(): Record<string, number> {
    const out: Record<string, number> = {};
    for (let i = 1; i < this.order.length; i++) {
      const a = this.order[i - 1];
      const b = this.order[i];
      out[`${a}→${b}`] = this.marks.get(b)! - this.marks.get(a)!;
    }
    if (this.order.length >= 2) {
      const first = this.order[0];
      const last = this.order[this.order.length - 1];
      out[`${first}→${last}`] = this.marks.get(last)! - this.marks.get(first)!;
    }
    return out;
  }

  log(sessionId: string): void {
    log.info({ sessionId, latencies: this.report() }, 'session latency');
  }
}
```

- [ ] **Step 4: Run, expect pass**

- [ ] **Step 5: Commit**

```bash
git add src/realtime/metrics.ts tests/realtime/metrics.test.ts
git commit -m "feat(realtime): latency tracker"
```

## Task M1.9: RealtimeBridge

**Files:**
- Create: `voice-assistant/src/realtime/realtimeBridge.ts`

> Integration-tested in M1.10. No unit test here; mocking both sides creates a fake without testing real behavior.

- [ ] **Step 1: Implement**

```ts
// src/realtime/realtimeBridge.ts
import type WebSocket from 'ws';
import { pino } from 'pino';
import { OpenAiRealtimeClient, type RealtimeEvent } from './openaiRealtimeClient.js';
import { resamplePcm16 } from './audio/resample.js';
import { pcm16ToBase64, base64ToPcm16 } from './audio/format.js';
import { encodeServerMessage, parseDeviceMessage } from './protocol.js';
import { LatencyTracker } from './metrics.js';
import type { RealtimeTool } from './toolAdapter.js';

const log = pino({ name: 'realtime-bridge' });

export interface BridgeDeps {
  apiKey: string;
  model: string;
  instructions: string;
  voice: string;
  tools: RealtimeTool[];
  runTool: (name: string, args: unknown) => Promise<string>;
}

export class RealtimeBridge {
  private openai: OpenAiRealtimeClient;
  private metrics = new LatencyTracker();
  private sessionId = Math.random().toString(36).slice(2, 10);

  constructor(
    private deviceWs: WebSocket,
    private deps: BridgeDeps,
  ) {
    this.openai = new OpenAiRealtimeClient({
      apiKey: deps.apiKey,
      model: deps.model,
      instructions: deps.instructions,
      voice: deps.voice,
      tools: deps.tools,
    });
  }

  async start(): Promise<void> {
    this.metrics.mark('bridge_start');
    await this.openai.connect();
    this.metrics.mark('openai_connected');

    this.openai.on((ev) => this.handleOpenAi(ev));

    this.deviceWs.on('message', (data, isBinary) => this.handleDevice(data, isBinary));
    this.deviceWs.on('close', () => {
      log.info({ sessionId: this.sessionId }, 'device closed');
      this.metrics.log(this.sessionId);
      this.openai.close();
    });

    this.sendDevice({ type: 'hello', audioOut: 'pcm' });
    this.sendDevice({ type: 'phase', value: 'idle' });
  }

  private handleDevice(data: WebSocket.RawData, isBinary: boolean): void {
    if (isBinary) {
      this.metrics.mark('first_audio_in');
      const pcm16k = data as Buffer;
      const pcm24k = resamplePcm16(pcm16k, 16000, 24000);
      this.openai.appendAudioPcm16Base64(pcm16ToBase64(pcm24k));
      return;
    }
    try {
      const msg = parseDeviceMessage(data.toString());
      log.debug({ msg }, 'device control msg');
      if (msg.type === 'interrupt') {
        this.openai.cancelResponse();
        this.sendDevice({ type: 'phase', value: 'listening' });
      } else if (msg.type === 'ping') {
        this.sendDevice({ type: 'pong' });
      }
    } catch (err) {
      log.warn({ err }, 'bad device control message');
    }
  }

  private handleOpenAi(ev: RealtimeEvent): void {
    switch (ev.type) {
      case 'input_audio_buffer.speech_started':
        this.sendDevice({ type: 'phase', value: 'listening' });
        break;
      case 'response.created':
        this.metrics.mark('thinking_started');
        this.sendDevice({ type: 'phase', value: 'thinking' });
        break;
      case 'response.audio.delta': {
        this.metrics.mark('first_audio_out');
        const pcm24k = base64ToPcm16((ev as any).delta);
        this.deviceWs.send(pcm24k, { binary: true });
        this.sendDevice({ type: 'phase', value: 'replying' });
        break;
      }
      case 'response.function_call_arguments.done':
        void this.handleToolCall(
          (ev as any).call_id,
          (ev as any).name,
          (ev as any).arguments,
        );
        break;
      case 'response.done':
        this.sendDevice({ type: 'phase', value: 'idle' });
        break;
      case 'error':
        log.error({ ev }, 'openai realtime error');
        this.sendDevice({ type: 'error', message: (ev as any).error?.message ?? 'unknown' });
        break;
    }
  }

  private async handleToolCall(callId: string, name: string, argsJson: string): Promise<void> {
    log.info({ name, callId }, 'tool call');
    try {
      const args = JSON.parse(argsJson);
      const result = await this.deps.runTool(name, args);
      this.openai.submitToolResult(callId, result);
    } catch (err) {
      this.openai.submitToolResult(
        callId,
        JSON.stringify({ error: (err as Error).message }),
      );
    }
  }

  private sendDevice(msg: Parameters<typeof encodeServerMessage>[0]): void {
    this.deviceWs.send(encodeServerMessage(msg));
  }
}
```

- [ ] **Step 2: Type-check**

Run: `npm run typecheck`
Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add src/realtime/realtimeBridge.ts
git commit -m "feat(realtime): RealtimeBridge — device WS ↔ OpenAI Realtime"
```

## Task M1.10: WS server with integration test

**Files:**
- Create: `voice-assistant/src/realtime/wsServer.ts`
- Create: `voice-assistant/src/realtime/index.ts`
- Create: `voice-assistant/tests/realtime/wsServer.test.ts`

- [ ] **Step 1: Write the failing test**

```ts
// tests/realtime/wsServer.test.ts
import { describe, it, expect, afterEach } from 'vitest';
import WebSocket, { WebSocketServer } from 'ws';
import { AddressInfo } from 'net';
import { startRealtimeServer } from '../../src/realtime/index.js';

let realtimeServer: { close: () => Promise<void> } | null = null;
let mockOpenAi: WebSocketServer | null = null;

afterEach(async () => {
  await realtimeServer?.close();
  realtimeServer = null;
  mockOpenAi?.close();
  mockOpenAi = null;
});

function startMockOpenAi(): Promise<{ port: number; events: any[]; sendEvent: (ev: any) => void }> {
  return new Promise((resolve) => {
    const events: any[] = [];
    let clientWs: WebSocket | null = null;
    const wss = new WebSocketServer({ port: 0 });
    mockOpenAi = wss;
    wss.on('connection', (ws) => {
      clientWs = ws;
      ws.on('message', (data) => events.push(JSON.parse(data.toString())));
    });
    wss.on('listening', () => {
      const addr = wss.address() as AddressInfo;
      resolve({
        port: addr.port,
        events,
        sendEvent: (ev) => clientWs?.send(JSON.stringify(ev)),
      });
    });
  });
}

describe('startRealtimeServer', () => {
  it('rejects unauthorized connections', async () => {
    realtimeServer = await startRealtimeServer({
      port: 0,
      token: 'secret',
      // @ts-expect-error — partial deps OK for this test
      buildBridgeDeps: async () => ({}),
    });
    const port = (realtimeServer as any).port;
    const ws = new WebSocket(`ws://127.0.0.1:${port}/voice`);
    await new Promise<void>((resolve) => {
      ws.on('close', (code) => {
        expect(code).toBe(4401);
        resolve();
      });
    });
  });

  it('accepts authorized connection and forwards audio to OpenAI', async () => {
    const mock = await startMockOpenAi();
    process.env.OPENAI_REALTIME_URL_OVERRIDE = `ws://127.0.0.1:${mock.port}`;
    realtimeServer = await startRealtimeServer({
      port: 0,
      token: 'secret',
      buildBridgeDeps: async () => ({
        apiKey: 'sk-fake',
        model: 'gpt-realtime',
        instructions: 'be brief',
        voice: 'alloy',
        tools: [],
        runTool: async () => 'ok',
      }),
    });
    const port = (realtimeServer as any).port;
    const ws = new WebSocket(`ws://127.0.0.1:${port}/voice`, {
      headers: { Authorization: 'Bearer secret' },
    });
    await new Promise<void>((resolve) => ws.on('open', resolve));
    const pcm = Buffer.alloc(320, 0);
    ws.send(pcm, { binary: true });
    await new Promise((r) => setTimeout(r, 100));
    expect(mock.events.some((e) => e.type === 'input_audio_buffer.append')).toBe(true);
    ws.close();
  });
});
```

- [ ] **Step 2: Run, expect fail**

Run: `npx vitest run tests/realtime/wsServer.test.ts`

- [ ] **Step 3: Implement wsServer + index**

```ts
// src/realtime/wsServer.ts
import { createServer, type Server } from 'node:http';
import { WebSocketServer } from 'ws';
import { pino } from 'pino';
import { verifyBearer } from './auth.js';
import { RealtimeBridge, type BridgeDeps } from './realtimeBridge.js';

const log = pino({ name: 'realtime-ws-server' });

export interface StartOptions {
  port: number;
  token: string;
  buildBridgeDeps: () => Promise<BridgeDeps>;
}

export interface RealtimeServer {
  port: number;
  close: () => Promise<void>;
}

export async function startRealtimeServer(opts: StartOptions): Promise<RealtimeServer> {
  const http: Server = createServer((_req, res) => {
    res.writeHead(404).end();
  });
  const wss = new WebSocketServer({ noServer: true });

  http.on('upgrade', (req, socket, head) => {
    if (req.url !== '/voice') {
      socket.destroy();
      return;
    }
    if (!verifyBearer(req.headers.authorization, opts.token)) {
      socket.write('HTTP/1.1 401 Unauthorized\r\n\r\n');
      socket.destroy();
      return;
    }
    wss.handleUpgrade(req, socket, head, async (ws) => {
      try {
        const deps = await opts.buildBridgeDeps();
        const bridge = new RealtimeBridge(ws, deps);
        await bridge.start();
      } catch (err) {
        log.error({ err }, 'failed to start bridge');
        ws.close(1011, 'bridge start failed');
      }
    });
  });

  // After WS upgrade is accepted, the auth check happens; reject path mismatch
  // by closing with 4401 so tests can assert.
  wss.on('connection', () => {});

  await new Promise<void>((resolve) => http.listen(opts.port, resolve));
  const port = (http.address() as { port: number }).port;
  log.info({ port }, 'realtime ws server listening');

  return {
    port,
    close: () =>
      new Promise<void>((resolve) => {
        wss.close();
        http.close(() => resolve());
      }),
  };
}
```

```ts
// src/realtime/index.ts
export { startRealtimeServer, type RealtimeServer } from './wsServer.js';
export type { BridgeDeps } from './realtimeBridge.js';
```

Adjust auth so unauth path produces a 4401 close on a WS upgrade rather than a 401 HTTP — the test expects `code === 4401`. Update wsServer.ts: accept upgrade, then close with 4401 if auth fails. Replace the `if (!verifyBearer)` branch in `upgrade` with:

```ts
wss.handleUpgrade(req, socket, head, async (ws) => {
  if (!verifyBearer(req.headers.authorization, opts.token)) {
    ws.close(4401, 'unauthorized');
    return;
  }
  try {
    const deps = await opts.buildBridgeDeps();
    const bridge = new RealtimeBridge(ws, deps);
    await bridge.start();
  } catch (err) {
    log.error({ err }, 'failed to start bridge');
    ws.close(1011, 'bridge start failed');
  }
});
```

Also, the openaiRealtimeClient should honor `process.env.OPENAI_REALTIME_URL_OVERRIDE` for tests. Edit `src/realtime/openaiRealtimeClient.ts`, replace the URL construction:

```ts
const base = process.env.OPENAI_REALTIME_URL_OVERRIDE ?? 'wss://api.openai.com/v1/realtime';
const url = `${base}?model=${encodeURIComponent(this.opts.model)}`;
```

- [ ] **Step 4: Run, expect pass**

Run: `npx vitest run tests/realtime/wsServer.test.ts`
Expected: PASS, 2/2.

- [ ] **Step 5: Commit**

```bash
git add src/realtime/wsServer.ts src/realtime/index.ts src/realtime/openaiRealtimeClient.ts tests/realtime/wsServer.test.ts
git commit -m "feat(realtime): WS server with auth + integration test"
```

## Task M1.11: Wire into existing CLI

**Files:**
- Modify: `voice-assistant/src/config.ts`
- Modify: `voice-assistant/src/cli/unified.ts`

- [ ] **Step 1: Read existing config.ts and unified.ts**

```bash
cd voice-assistant
cat src/config.ts | head -80
cat src/cli/unified.ts | head -80
```

- [ ] **Step 2: Add config keys**

Append to `src/config.ts` (inside the env parsing — exact location depends on current shape; add alongside other env reads):

```ts
export const realtimeConfig = {
  enabled: process.env.REALTIME_ENABLED === '1',
  port: Number(process.env.REALTIME_PORT ?? '3001'),
  token: process.env.VA_DEVICE_TOKEN ?? '',
  model: process.env.OPENAI_REALTIME_MODEL ?? 'gpt-realtime',
  voice: process.env.OPENAI_REALTIME_VOICE ?? 'alloy',
};
```

- [ ] **Step 3: Boot realtime server from unified.ts**

In `src/cli/unified.ts`, after the existing agent/MCP initialization, add:

```ts
import { startRealtimeServer } from '../realtime/index.js';
import { mcpToolsToRealtime } from '../realtime/toolAdapter.js';
import { realtimeConfig } from '../config.js';

// ... after mcpClient is ready and system prompt is built:
if (realtimeConfig.enabled) {
  if (!realtimeConfig.token) {
    throw new Error('REALTIME_ENABLED=1 but VA_DEVICE_TOKEN is empty');
  }
  await startRealtimeServer({
    port: realtimeConfig.port,
    token: realtimeConfig.token,
    buildBridgeDeps: async () => {
      const mcpTools = await mcpClient.listTools();
      return {
        apiKey: process.env.OPENAI_API_KEY!,
        model: realtimeConfig.model,
        voice: realtimeConfig.voice,
        instructions: systemPrompt,
        tools: mcpToolsToRealtime(mcpTools),
        runTool: (name, args) => mcpClient.callTool(name, args),
      };
    },
  });
}
```

> Note: exact import names for `mcpClient.listTools` / `callTool` and `systemPrompt` must match what's already exported in this repo. Read [src/mcp/haMcpClient.ts](../../voice-assistant/src/mcp/haMcpClient.ts) and [src/agent/systemPrompt.ts](../../voice-assistant/src/agent/systemPrompt.ts) before writing — adapt method names but keep behavior identical.

- [ ] **Step 4: Type-check + lint**

```bash
npm run typecheck
npm run lint
```

Expected: pass.

- [ ] **Step 5: Commit**

```bash
git add src/config.ts src/cli/unified.ts
git commit -m "feat(realtime): wire realtime server into unified CLI"
```

## Task M1.12: CLI smoke client

**Files:**
- Create: `voice-assistant/src/cli/realtimeSmoke.ts`

- [ ] **Step 1: Implement**

```ts
// src/cli/realtimeSmoke.ts
import WebSocket from 'ws';
import { readFileSync, writeFileSync, createWriteStream } from 'node:fs';

const wavPath = process.argv[2];
const token = process.env.VA_DEVICE_TOKEN!;
const port = Number(process.env.REALTIME_PORT ?? '3001');
const host = process.env.REALTIME_HOST ?? '127.0.0.1';

if (!wavPath || !token) {
  console.error('usage: VA_DEVICE_TOKEN=... node src/cli/realtimeSmoke.ts <wav>');
  process.exit(2);
}

function readWavPcm16Mono16k(path: string): Buffer {
  const buf = readFileSync(path);
  // Naive: assume canonical WAV header is 44 bytes. Real wav parsing later.
  const fmtChunk = buf.indexOf('fmt ');
  if (fmtChunk < 0) throw new Error('not a wav');
  const sampleRate = buf.readUInt32LE(fmtChunk + 12);
  const channels = buf.readUInt16LE(fmtChunk + 10);
  if (sampleRate !== 16000 || channels !== 1) {
    throw new Error(`need 16kHz mono, got ${sampleRate}Hz ${channels}ch`);
  }
  const dataIdx = buf.indexOf('data');
  const dataLen = buf.readUInt32LE(dataIdx + 4);
  return buf.subarray(dataIdx + 8, dataIdx + 8 + dataLen);
}

const pcm = readWavPcm16Mono16k(wavPath);
const out = createWriteStream('out.pcm');
let firstAudioOutTs: number | null = null;
const startTs = Date.now();

const ws = new WebSocket(`ws://${host}:${port}/voice`, {
  headers: { Authorization: `Bearer ${token}` },
});

ws.on('open', () => {
  console.log(`connected to ${host}:${port}, sending ${pcm.length} bytes`);
  ws.send(JSON.stringify({ type: 'start' }));
  // Send in 20ms chunks (640 bytes @ 16kHz pcm16)
  const chunkBytes = 640;
  let i = 0;
  const timer = setInterval(() => {
    if (i >= pcm.length) {
      clearInterval(timer);
      console.log('all audio sent, waiting for response...');
      return;
    }
    const slice = pcm.subarray(i, Math.min(i + chunkBytes, pcm.length));
    ws.send(slice, { binary: true });
    i += chunkBytes;
  }, 20);
});

ws.on('message', (data, isBinary) => {
  if (isBinary) {
    if (firstAudioOutTs === null) {
      firstAudioOutTs = Date.now();
      console.log(`first audio out: ${firstAudioOutTs - startTs}ms`);
    }
    out.write(data as Buffer);
  } else {
    console.log('control:', data.toString());
  }
});

ws.on('close', (code) => {
  console.log(`closed: ${code}`);
  out.end();
  console.log('wrote out.pcm (PCM16 24kHz mono)');
  process.exit(0);
});

setTimeout(() => ws.close(), 15000);
```

- [ ] **Step 2: Add npm script**

In `package.json` scripts:

```json
"realtime:smoke": "node src/cli/realtimeSmoke.ts"
```

- [ ] **Step 3: Run end-to-end manually**

```bash
# Terminal 1
cd voice-assistant
REALTIME_ENABLED=1 VA_DEVICE_TOKEN=$(openssl rand -hex 16) OPENAI_API_KEY=sk-... npm run start

# Terminal 2 — record a short utterance into sample.wav (16kHz mono pcm16)
# e.g. sox -d -c 1 -r 16000 -b 16 sample.wav trim 0 3
VA_DEVICE_TOKEN=<same> npm run realtime:smoke -- sample.wav

# Verify
ffplay -f s16le -ar 24000 -ac 1 out.pcm
```

Expected: smoke client connects, latency printed, `out.pcm` contains audible reply.

- [ ] **Step 4: Commit**

```bash
git add src/cli/realtimeSmoke.ts package.json
git commit -m "feat(realtime): CLI smoke client for end-to-end testing"
```

## Task M1.13: M1 verification

- [ ] Run `npm test` — expect all green.
- [ ] Run `npm run typecheck` — expect clean.
- [ ] Run `npm run lint` — expect clean.
- [ ] Run the manual smoke from M1.12 step 3 and confirm latency is reasonable (< 2s start→first audio out for a short utterance).
- [ ] Push branch.

---

# M2 — Firmware prototype (raw PCM end-to-end)

**Goal:** New `va_client` ESPHome component sends raw PCM to va, plays raw PCM back. Wake-word triggered, LED phases driven by va. Old yaml stays unchanged in this milestone — new `va-direct` yaml lives alongside.

**Definition of done:**
- `esphome compile home-assistant-voice.va-direct.yaml` succeeds.
- Device flashed; saying "okay nabu, what time is it" yields a spoken reply.
- LED phases (listening / thinking / replying / idle) match conversation state.

## Task M2.1: Component skeleton

**Files:**
- Create: `esphome/components/va_client/__init__.py`
- Create: `esphome/components/va_client/va_client.h`
- Create: `esphome/components/va_client/va_client.cpp`

- [ ] **Step 1: Create `__init__.py`**

```python
# esphome/components/va_client/__init__.py
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import microphone, speaker
from esphome.const import CONF_ID, CONF_URL

CODEOWNERS = ["@maxmaxme"]
DEPENDENCIES = ["network", "microphone", "speaker"]

va_client_ns = cg.esphome_ns.namespace("va_client")
VaClient = va_client_ns.class_("VaClient", cg.Component)

CONF_TOKEN = "token"
CONF_MICROPHONE = "microphone"
CONF_SPEAKER = "speaker"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(VaClient),
        cv.Required(CONF_URL): cv.string,
        cv.Required(CONF_TOKEN): cv.string,
        cv.Required(CONF_MICROPHONE): cv.use_id(microphone.Microphone),
        cv.Required(CONF_SPEAKER): cv.use_id(speaker.Speaker),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_url(config[CONF_URL]))
    cg.add(var.set_token(config[CONF_TOKEN]))
    mic = await cg.get_variable(config[CONF_MICROPHONE])
    spk = await cg.get_variable(config[CONF_SPEAKER])
    cg.add(var.set_microphone(mic))
    cg.add(var.set_speaker(spk))
```

- [ ] **Step 2: Create header**

```cpp
// esphome/components/va_client/va_client.h
#pragma once
#include "esphome/core/component.h"
#include "esphome/components/microphone/microphone.h"
#include "esphome/components/speaker/speaker.h"
#include <string>

namespace esphome {
namespace va_client {

enum class Phase { Idle, Listening, Thinking, Replying };

class VaClient : public Component {
 public:
  void set_url(const std::string &url) { url_ = url; }
  void set_token(const std::string &token) { token_ = token; }
  void set_microphone(microphone::Microphone *m) { mic_ = m; }
  void set_speaker(speaker::Speaker *s) { speaker_ = s; }

  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

  void start_session();
  void send_interrupt();
  Phase phase() const { return phase_; }

 private:
  std::string url_;
  std::string token_;
  microphone::Microphone *mic_{nullptr};
  speaker::Speaker *speaker_{nullptr};
  Phase phase_{Phase::Idle};

  void connect_();
  void on_ws_open_();
  void on_ws_message_(const uint8_t *data, size_t len, bool binary);
  void on_ws_close_(int code);
  void on_mic_data_(const std::vector<int16_t> &samples);
  void set_phase_(Phase p);
  void schedule_reconnect_();

  void *ws_client_{nullptr};  // esp_websocket_client_handle_t (opaque to header)
  uint32_t reconnect_delay_ms_{1000};
};

}  // namespace va_client
}  // namespace esphome
```

- [ ] **Step 3: Create implementation skeleton**

```cpp
// esphome/components/va_client/va_client.cpp
#include "va_client.h"
#include "esphome/core/log.h"

extern "C" {
#include "esp_websocket_client.h"
#include "esp_event.h"
}

namespace esphome {
namespace va_client {

static const char *const TAG = "va_client";

void VaClient::setup() {
  ESP_LOGI(TAG, "va_client setup: url=%s", url_.c_str());
  // Subscribe to mic data
  if (mic_) {
    mic_->add_data_callback([this](const std::vector<int16_t> &s) { this->on_mic_data_(s); });
  }
  this->connect_();
}

void VaClient::loop() {
  // websocket client runs in its own task; nothing to do here for now
}

void VaClient::connect_() {
  esp_websocket_client_config_t cfg = {};
  cfg.uri = url_.c_str();
  std::string auth = "Authorization: Bearer " + token_ + "\r\n";
  cfg.headers = auth.c_str();
  cfg.reconnect_timeout_ms = 5000;

  auto *handle = esp_websocket_client_init(&cfg);
  ws_client_ = handle;
  esp_websocket_register_events(
      handle, WEBSOCKET_EVENT_ANY,
      [](void *arg, esp_event_base_t, int32_t event_id, void *event_data) {
        auto *self = static_cast<VaClient *>(arg);
        auto *data = static_cast<esp_websocket_event_data_t *>(event_data);
        switch (static_cast<esp_websocket_event_id_t>(event_id)) {
          case WEBSOCKET_EVENT_CONNECTED:
            self->on_ws_open_();
            break;
          case WEBSOCKET_EVENT_DATA:
            self->on_ws_message_(reinterpret_cast<const uint8_t *>(data->data_ptr),
                                 data->data_len, data->op_code == 0x02);
            break;
          case WEBSOCKET_EVENT_DISCONNECTED:
          case WEBSOCKET_EVENT_CLOSED:
            self->on_ws_close_(0);
            break;
          default:
            break;
        }
      },
      this);
  esp_websocket_client_start(handle);
}

void VaClient::on_ws_open_() {
  ESP_LOGI(TAG, "ws connected");
  set_phase_(Phase::Idle);
  const char *hello = "{\"type\":\"start\"}";
  esp_websocket_client_send_text(static_cast<esp_websocket_client_handle_t>(ws_client_),
                                 hello, strlen(hello), portMAX_DELAY);
}

void VaClient::on_ws_message_(const uint8_t *data, size_t len, bool binary) {
  if (binary) {
    // PCM16 24kHz mono — feed to speaker
    if (speaker_) {
      speaker_->play(data, len);
    }
    return;
  }
  std::string msg(reinterpret_cast<const char *>(data), len);
  ESP_LOGD(TAG, "control: %s", msg.c_str());
  if (msg.find("\"replying\"") != std::string::npos) set_phase_(Phase::Replying);
  else if (msg.find("\"listening\"") != std::string::npos) set_phase_(Phase::Listening);
  else if (msg.find("\"thinking\"") != std::string::npos) set_phase_(Phase::Thinking);
  else if (msg.find("\"idle\"") != std::string::npos) set_phase_(Phase::Idle);
}

void VaClient::on_ws_close_(int) {
  ESP_LOGW(TAG, "ws closed, reconnect in %u ms", reconnect_delay_ms_);
  schedule_reconnect_();
}

void VaClient::on_mic_data_(const std::vector<int16_t> &samples) {
  if (!ws_client_) return;
  if (!esp_websocket_client_is_connected(static_cast<esp_websocket_client_handle_t>(ws_client_))) return;
  esp_websocket_client_send_bin(
      static_cast<esp_websocket_client_handle_t>(ws_client_),
      reinterpret_cast<const char *>(samples.data()),
      samples.size() * sizeof(int16_t), portMAX_DELAY);
}

void VaClient::send_interrupt() {
  if (!ws_client_) return;
  const char *m = "{\"type\":\"interrupt\"}";
  esp_websocket_client_send_text(static_cast<esp_websocket_client_handle_t>(ws_client_),
                                 m, strlen(m), portMAX_DELAY);
}

void VaClient::start_session() {
  // Mic streaming is always-on once connected; this is a hook for yaml
  ESP_LOGD(TAG, "start_session triggered");
}

void VaClient::set_phase_(Phase p) {
  phase_ = p;
  ESP_LOGD(TAG, "phase: %d", static_cast<int>(p));
}

void VaClient::schedule_reconnect_() {
  // esp_websocket_client handles reconnect via its own timer; this stub is for future custom backoff
}

}  // namespace va_client
}  // namespace esphome
```

- [ ] **Step 4: Commit**

```bash
cd home-assistant-voice-pe
git add esphome/components/va_client
git commit -m "feat(va_client): component skeleton with WS client + mic→speaker passthrough"
```

## Task M2.2: New top-level yaml

**Files:**
- Create: `home-assistant-voice-pe/home-assistant-voice.va-direct.yaml`

- [ ] **Step 1: Read existing yaml carefully**

```bash
cd home-assistant-voice-pe
wc -l home-assistant-voice.yaml
```

Read these blocks before writing the new file:
- `substitutions`
- `esphome`, `esp32`, `wifi`, `improv_serial`, `ota`, `logger`, `web_server` (if present)
- `external_components`
- `i2s_audio:` (≈ 1503)
- `microphone:` (≈ 1518)
- `speaker:` (≈ 1530, the `i2s_audio` one only — the mixer is removed)
- `voice_kit:` (XMOS — keep)
- `micro_wake_word:` (≈ 1695)
- LED/button blocks (keep, but rebind phase actions)

- [ ] **Step 2: Write va-direct yaml**

Copy `home-assistant-voice.yaml` to `home-assistant-voice.va-direct.yaml`, then in the copy:

1. Remove the `api:` block.
2. Remove the entire `voice_assistant:` block.
3. Remove the mixer-based `speaker:` sources (`announcement_input`, `media_input`) and the `external_media_player`.
4. Replace LED phase triggers (currently fired by `voice_assistant` events) with triggers on `va_client` phase changes — use a `lambda` that polls `va_client->phase()` and updates LEDs, OR add an `on_phase` automation hook in the component (deferred to M3 if too much).
5. Add `external_components` entry for the local `va_client` component:

   ```yaml
   external_components:
     - source:
         type: local
         path: esphome/components
       components: [voice_kit, va_client]
   ```

6. Add `va_client` block:

   ```yaml
   substitutions:
     va_url: "ws://192.168.1.42:3001/voice"
     va_token: !secret va_device_token

   va_client:
     id: va
     url: ${va_url}
     token: ${va_token}
     microphone: mic_source_ch0  # use the existing mic id
     speaker: speaker_i2s         # use the existing speaker id
   ```

7. In the `micro_wake_word:` block, replace the `on_wake_word_detected:` action that previously called `voice_assistant.start` with a lambda call to `id(va).start_session();`.
8. For the "stop" wake word, replace with `id(va).send_interrupt();`.

- [ ] **Step 3: Compile**

```bash
cd home-assistant-voice-pe
# Substitute path / secrets file as appropriate
esphome compile home-assistant-voice.va-direct.yaml
```

Expected: clean compile. Fix any include / typing errors iteratively. Do NOT proceed until compile is clean.

- [ ] **Step 4: Commit**

```bash
git add home-assistant-voice.va-direct.yaml
git commit -m "feat(firmware): va-direct yaml — voice-only flavour using va_client"
```

## Task M2.3: Flash + manual smoke

- [ ] **Step 1: Set up secrets**

```bash
cd home-assistant-voice-pe
# Append to local secrets file (NOT committed):
echo "va_device_token: \"<same token used by va>\"" >> secrets.yaml
```

- [ ] **Step 2: Flash via USB**

```bash
esphome run home-assistant-voice.va-direct.yaml --device <serial-port>
```

- [ ] **Step 3: Manual test**

1. Confirm device boots, connects to wifi.
2. Look at logs: WS connects to va, "phase: 0" prints.
3. Say "okay nabu, what time is it".
4. Expect: listening LED → thinking LED → replying LED → audio reply.
5. Note latency from end-of-speech to first audio (use Pi-side va logs from `LatencyTracker`).

- [ ] **Step 4: Document findings**

Append a short section to the spec's "Open questions" section with measured latency baseline and any issues observed.

```bash
git add docs/superpowers/specs/2026-05-25-voice-pe-direct-va-streaming-design.md
git commit -m "docs: M2 baseline latency measurements"
```

---

# M3 — Polish + reliability

**Goal:** Stabilise the M2 experience. Stop wake-word becomes a proper
interrupt (not a new session), echo cancellation gets verified end-to-end,
on-device latency metrics, graceful error sounds, longer-form testing.

> **Codec note.** M3 was originally "Opus + interrupt + reliability".
> Opus is dropped: on a LAN deployment the 640 Kbit/s raw PCM16 stream
> is trivial for wifi and the ESP32-S3 is busy enough running mww + I2S
> + WS without an extra ~10 % core for libopus. See the spec's "Codec
> policy" update.
>
> Several items the original M3 plan called out have already landed
> in M2 stabilisation commits and are tracked under "Done before M3
> started" at the bottom — they don't need separate tasks here.

**Definition of done:**
- Saying "stop" while the AI is replying cancels the active turn,
  flushes the audio buffer, drops phase back to idle. NOT a fresh
  wake-word session.
- A short conversation (≥5 turns) plays cleanly without echo, without
  the device hearing its own TTS, without dropped chunks.
- A device log line at end of each turn summarises the relevant timings
  (`wake → first_audio_in → speech_stopped → first_audio_out → drained`).
- When va is unreachable, the device plays a discoverable error chime
  through `play_sound` (not just LED) so the user knows something is
  wrong without reading logs.

## Task M3.1: Stop wake-word distinguishes from okay_nabu

**Files:**
- Modify: `home-assistant-voice-pe/home-assistant-voice.va-direct.yaml`
  (the `micro_wake_word.on_wake_word_detected:` action chain)

The chain currently fires `id(va)->start_session()` for ANY detected
wake word, including `stop`. We want `stop` to call `send_interrupt()`
instead.

ESPHome's `on_wake_word_detected` passes the detected model's id via a
`wake_word` variable. Branch on it.

- [ ] **Step 1: Read current chain**

```bash
cd /Users/mlepekha/Developer/home/home-assistant-voice-pe
grep -n -A 30 "on_wake_word_detected:" home-assistant-voice.va-direct.yaml | head -50
```

- [ ] **Step 2: Add a `wake_word == "stop"` branch**

Replace the existing `then` body with:

```yaml
on_wake_word_detected:
  - if:
      condition:
        lambda: 'return wake_word == "stop";'
      then:
        # The "stop" word is only armed during reply; it cancels the
        # in-flight turn server-side and flushes our local audio queue.
        - lambda: 'id(va)->send_interrupt();'
      else:
        # The previous full cascade (timer check, mute check, wake-sound,
        # start_session). Keep it intact for okay_nabu / hey_jarvis /
        # hey_mycroft.
        - <existing cascade>
```

The exact YAML structure depends on the current chain — wrap the
existing body in the `else:` branch of the new `if`.

- [ ] **Step 3: Verify "stop" only fires during reply**

In yaml, "stop" is configured as `internal: true` and we want it armed
only while `phase=replying`. The existing `activate_stop_word_once`
script does this on `media_player.is_announcing` — but our TTS doesn't
go through media_player. Wire it differently:

  - On the va_client `on_phase` trigger, when `phase == "replying"`:
    `micro_wake_word.enable_model: stop`
  - When `phase == "idle"`: `micro_wake_word.disable_model: stop`

Update the `va_client.on_phase` yaml block to add these.

- [ ] **Step 4: Implement audio queue flush in `send_interrupt`**

In `va_client.cpp`, `send_interrupt()` currently only sends the WS text
message. Also flush our PSRAM audio queue so playback stops immediately:

```cpp
void VaClient::send_interrupt() {
  if (!this->ws_connected_ || this->ws_handle_ == nullptr) {
    ESP_LOGW(TAG, "send_interrupt: WS not connected");
    return;
  }
  const char msg[] = "{\"type\":\"interrupt\"}";
  auto handle = static_cast<esp_websocket_client_handle_t>(this->ws_handle_);
  esp_websocket_client_send_text(handle, msg, sizeof(msg) - 1, portMAX_DELAY);
  // Drop everything in the playback queue. The speaker's own ring will
  // play out its ~500 ms residual, but that's fine — interrupt UX
  // expects "stop talking", not "absolute silence right now".
  this->audio_head_ = this->audio_tail_ = 0;
  this->audio_fill_ = 0;
  ESP_LOGI(TAG, "send_interrupt — WS msg sent, queue flushed");
}
```

- [ ] **Step 5: Manual test**

Flash, say "okay nabu, tell me a long story". When AI starts replying,
say "stop". Expected:
- Within ~300 ms, audio stops (modulo the ~500 ms speaker tail).
- LED returns to idle (with proper drain-deferred timing).
- No new session starts — wake word doesn't ding.

- [ ] **Step 6: Commit**

```bash
git add esphome/components/va_client/va_client.cpp \
        home-assistant-voice.va-direct.yaml
git commit -m "feat(va_client): stop wake-word as interrupt + audio queue flush"
```

## Task M3.2: Verify XMOS AEC so the mic doesn't hear the speaker

**Files:**
- Likely none; this is a debugging / configuration task. If we find
  the XMOS reference path isn't wired correctly, edit `voice_kit:` or
  `i2s_audio:` blocks in `home-assistant-voice.va-direct.yaml`.

The Voice PE's XMOS DSP runs AEC against a reference signal taken from
the I2S output bus. With the original `voice_assistant` + `media_player`
chain, that was straightforward. After we ripped out media_player and
rebuilt the speaker chain, it's worth verifying the AEC reference path
still works.

- [ ] **Step 1: Test setup**

With the device fully booted and idle:
1. Open serial logs.
2. Play loud TTS (say "okay nabu, count from one to ten slowly").
3. While AI is replying, stay silent and don't move.

- [ ] **Step 2: Check for spurious VAD events**

In va's docker logs (`docker logs voice-assistant --tail=100`),
look for events arriving DURING the reply:
- `input_audio_buffer.speech_started` while the device is supposed to
  be replying = mic picked up the speaker → AEC isn't working.

If clean → AEC works, skip the rest of this task. Mark done.

- [ ] **Step 3: If AEC failing, inspect**

Likely fixes:
- Confirm `voice_kit.pipeline_stages` config matches the original yaml.
- Check XMOS firmware version (`voice_kit:045: XMOS firmware version`
  log line) is `1.3.1`.
- The reference path may need explicit i2s loopback config in the
  yaml — compare with `home-assistant-voice.yaml` original.

- [ ] **Step 4: Document findings**

Append findings to the spec's "Open questions" or risks section.

## Task M3.3: On-device latency metrics

**Files:**
- Modify: `esphome/components/va_client/va_client.cpp`

va_client already logs many state transitions. Add a per-turn summary
log so we can read latency without correlating timestamps by hand.

- [ ] **Step 1: Add per-turn timestamps**

In `va_client.h`, add fields for the four anchor moments of a turn:

```cpp
uint32_t turn_t_wake_{0};
uint32_t turn_t_listening_{0};
uint32_t turn_t_thinking_{0};
uint32_t turn_t_first_audio_out_{0};
```

In `va_client.cpp`:
- `start_session()` → `turn_t_wake_ = millis(); reset others to 0;`
- `set_phase_("listening")` (first time after start_session) →
  `turn_t_listening_ = millis();`
- `set_phase_("thinking")` → `turn_t_thinking_ = millis();`
- `handle_binary_()` first call after `turn_t_thinking_` → record
  `turn_t_first_audio_out_ = millis();`
- `set_phase_("idle")` (the deferred fire, after drain) → log the
  summary:

```cpp
ESP_LOGI(TAG, "turn timings: wake→listening=%ums listening→thinking=%ums "
              "thinking→first_audio=%ums first_audio→done=%ums",
         turn_t_listening_ - turn_t_wake_,
         turn_t_thinking_ - turn_t_listening_,
         turn_t_first_audio_out_ - turn_t_thinking_,
         millis() - turn_t_first_audio_out_);
```

- [ ] **Step 2: Manual test**

Run a few representative utterances:
- "what time is it" (no tool call)
- "turn on the kitchen light" (one MCP call)
- "tell me a joke" (longer reply)

Read the summary lines. Record three baseline numbers in the spec under
a new "Performance" section.

- [ ] **Step 3: Commit**

```bash
git add esphome/components/va_client/
git commit -m "feat(va_client): per-turn latency summary log"
```

## Task M3.4: Audible error when va is unreachable

**Files:**
- Modify: `esphome/components/va_client/va_client.cpp` (or)
- Modify: `home-assistant-voice.va-direct.yaml`

Right now if the WS to va fails, the LED goes to red twinkle (via
`control_leds_no_ha_connection_state`) and the user sees nothing else
useful. The original behaviour played `error_cloud_expired.mp3` via
`play_sound` on cloud-auth failure. Replicate that for va connection
failure.

- [ ] **Step 1: Decide trigger**

After N consecutive reconnect failures (e.g. 5 → ~10 minutes of
backoff), play the error sound exactly once. Reset the counter on
successful connect.

- [ ] **Step 2: Add a `on_repeated_failure` automation hook**

In `va_client/__init__.py`, add a new automation trigger:
`OnRepeatedFailureTrigger`. In `va_client.cpp`, increment a counter on
each reconnect failure; on counter == 5, fire the trigger.

- [ ] **Step 3: Wire into yaml**

```yaml
va_client:
  ...
  on_repeated_failure:
    - script.execute:
        id: play_sound
        priority: true
        sound_file: "error_cloud_expired_sound"
```

- [ ] **Step 4: Manual test**

Stop va docker container on the Pi. Wait until 5 reconnect attempts have
elapsed. Verify error chime plays once, not on every failure.

- [ ] **Step 5: Commit**

```bash
git add esphome/components/va_client/ home-assistant-voice.va-direct.yaml
git commit -m "feat(va_client): audible error on repeated connection failure"
```

## Task M3.5: Stability soak

Run an unattended soak test:
- Leave device idle for 30 minutes; confirm no WS drops, no spurious
  wake-word triggers.
- Have a normal conversation (5+ turns) every 5 minutes for ~30 min.
- Inspect free heap (`debug:` component logs it) trend over the run —
  should NOT grow.

No code changes expected; goal is to catch any leak or pathological
case before declaring M3 done.

## Done before M3 started

These items appeared in earlier drafts of the M3 plan. They landed
during M2 stabilisation already, listed here so we don't redo them:

- Reconnect with exponential backoff and reset-on-open
  (`fix(va_client): coalesce reconnect on cascading WS event burst`).
- Follow-up dialog window (5 s) after AI reply with drain timing
  (`fix(va_client): add follow-up dialog window with proper drain timing`).
- LED idle transition deferred until speaker actually drained
  (`feat(va_client): defer LED idle transition until speaker actually drained`).
- WS continuation frame handling.
- PSRAM audio queue + draining via `speaker.play()` from `loop()`.
- Per-phase mic streaming gate to prevent self-listening.

---

# M4 — Production deployment

**Goal:** Ship to production stack — docker-compose updated, env wired, parallel with old pipeline as fallback.

## Task M4.1: docker-compose port + env

**Files:**
- Modify: `home-infra/docker-compose.yml`
- Modify: `home-infra/.env.example` (and `.env` locally)

- [ ] **Step 1: Read current compose**

```bash
cd home-infra
grep -A 10 voice-assistant docker-compose.yml
```

- [ ] **Step 2: Expose port + add env**

Edit the `voice-assistant` service:

```yaml
services:
  voice-assistant:
    # ... existing config ...
    ports:
      - "3000:3000"
      - "3001:3001"  # realtime WS
    environment:
      # ... existing env ...
      REALTIME_ENABLED: "${REALTIME_ENABLED:-1}"
      REALTIME_PORT: "3001"
      VA_DEVICE_TOKEN: "${VA_DEVICE_TOKEN}"
      OPENAI_REALTIME_MODEL: "${OPENAI_REALTIME_MODEL:-gpt-realtime}"
      OPENAI_REALTIME_VOICE: "${OPENAI_REALTIME_VOICE:-alloy}"
```

In `.env.example`:

```
REALTIME_ENABLED=1
VA_DEVICE_TOKEN=
OPENAI_REALTIME_MODEL=gpt-realtime
OPENAI_REALTIME_VOICE=alloy
```

- [ ] **Step 3: Generate and set the token on the Pi**

```bash
# on the Pi
openssl rand -hex 32  # paste into ~/home-infra/.env as VA_DEVICE_TOKEN
```

- [ ] **Step 4: Deploy**

```bash
cd ~/home-infra
./update.sh
docker compose logs voice-assistant | grep -i realtime
```

Expected: `realtime ws server listening port=3001`.

- [ ] **Step 5: Commit**

```bash
git add docker-compose.yml .env.example
git commit -m "feat(infra): expose voice-assistant :3001 realtime endpoint"
```

## Task M4.2: Flash device with prod URL

**Files:**
- Modify: `home-assistant-voice-pe/secrets.yaml` (local only)
- Modify: `home-assistant-voice-pe/home-assistant-voice.va-direct.yaml`

- [ ] **Step 1: Set production WS URL**

In yaml substitutions, point to the Pi's LAN IP (or mDNS name):

```yaml
substitutions:
  va_url: "ws://homepi.local:3001/voice"
```

- [ ] **Step 2: OTA flash**

```bash
cd home-assistant-voice-pe
esphome run home-assistant-voice.va-direct.yaml  # OTA over network
```

- [ ] **Step 3: Smoke**

Confirm wake word → reply with real prod va.

- [ ] **Step 4: Commit**

```bash
git add home-assistant-voice.va-direct.yaml
git commit -m "feat(firmware): point va_url at production Pi"
```

## Task M4.3: Parallel-run + measure

- [ ] **Step 1: Keep old yaml available**

`home-assistant-voice.yaml` stays in repo and is still flashable for rollback.

- [ ] **Step 2: Compare latency**

Run the same 10 test utterances against both pipelines (old via HA, new via va direct). Record results.

- [ ] **Step 3: Document**

Append a `Performance` section to the spec with measured numbers.

- [ ] **Step 4: Commit**

```bash
git add docs/superpowers/specs/2026-05-25-voice-pe-direct-va-streaming-design.md
git commit -m "docs: M4 latency comparison old vs direct"
```

---

# M5 — Cleanup

**Goal:** Remove the old pipeline, rename yaml, update CLAUDE.md across repos.

**Only run M5 after living with M4 for at least a week without issues.**

## Task M5.1: Delete old yaml

**Files:**
- Delete: `home-assistant-voice-pe/home-assistant-voice.yaml`
- Rename: `home-assistant-voice.va-direct.yaml` → `home-assistant-voice.yaml`

- [ ] **Step 1: Remove + rename**

```bash
cd home-assistant-voice-pe
git rm home-assistant-voice.yaml
git mv home-assistant-voice.va-direct.yaml home-assistant-voice.yaml
```

- [ ] **Step 2: Update references**

```bash
grep -r "va-direct" .
```

Fix any docs / CI / scripts pointing at the old name.

- [ ] **Step 3: Compile**

```bash
esphome compile home-assistant-voice.yaml
```

- [ ] **Step 4: Commit**

```bash
git add -A
git commit -m "refactor(firmware): make va-direct the sole top-level yaml"
```

## Task M5.2: Update README

**Files:**
- Modify: `home-assistant-voice-pe/README.md`

- [ ] **Step 1: Note that this fork is no longer drop-in compatible with stock Voice PE**

Add a section at the top of the README:

```markdown
> **Note:** This fork is NOT the stock Home Assistant Voice PE firmware.
> It bypasses the Home Assistant voice pipeline entirely and streams audio
> directly to a [voice-assistant](https://github.com/maxmaxme/voice-assistant)
> backend over WebSocket, backed by OpenAI Realtime API. It does not act
> as a media_player and cannot play music. See
> `docs/superpowers/specs/2026-05-25-voice-pe-direct-va-streaming-design.md`
> for the design.
```

- [ ] **Step 2: Commit**

```bash
git add README.md
git commit -m "docs: note this fork is voice-only, not stock Voice PE"
```

## Task M5.3: Update CLAUDE.md files

**Files:**
- Modify: `home/CLAUDE.md` (umbrella)
- Modify: `voice-assistant/CLAUDE.md`
- Modify: `home-assistant-voice-pe/CLAUDE.md` (create if missing)
- Modify: `home-infra/CLAUDE.md`

- [ ] **Step 1: Umbrella CLAUDE.md**

Update the "Cross-repo wiring" diagram and the table under "Where each kind of change goes":
- Add: `Change Voice PE ↔ va WS contract → all three: voice-assistant + home-assistant-voice-pe + home-infra docs`
- Mark that HA is no longer in the voice path (only tools via MCP).

- [ ] **Step 2: voice-assistant CLAUDE.md**

Document the new `src/realtime/` module structure and `REALTIME_*` env vars.

- [ ] **Step 3: home-assistant-voice-pe CLAUDE.md**

Create if missing; describe the `va_client` component, the active yaml, and that this is no longer a stock Voice PE.

- [ ] **Step 4: home-infra CLAUDE.md**

Add port 3001 and `VA_DEVICE_TOKEN` to the env reference.

- [ ] **Step 5: Commit each repo separately**

```bash
# umbrella isn't a git repo (per CLAUDE.md), so just save the file
# each subrepo gets its own commit
```

---

# Self-Review Checklist (for the plan author)

Verify before handing off:

- [ ] Every M-section has a clear DoD that can be checked without ambiguity.
- [ ] Every code step shows the actual code, not "implement the function".
- [ ] File paths are explicit and absolute relative to the repo root.
- [ ] Tests come before implementation (TDD) for all TypeScript code.
- [ ] Firmware tasks have manual verification steps since they can't be unit-tested on hardware here.
- [ ] Method names in later tasks match earlier definitions (`start_session`, `send_interrupt`, `handleDevice`, etc.).
- [ ] No `TBD`, `TODO`, "similar to" placeholders.
- [ ] Each milestone ends with a working, deployable state.

# Execution Note

Some firmware steps depend on hardware-specific details (exact pin IDs, mic source IDs, LED action references) that live in the current 1900-line `home-assistant-voice.yaml`. **Read the existing yaml carefully before each firmware task and adapt id references to what's actually there.** The plan names placeholders like `mic_source_ch0` and `speaker_i2s`; the real ids may differ.
