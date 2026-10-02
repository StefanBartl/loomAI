// T3: custom OpenAI-compatible models are routed to their own endpoint by the
// real binary. A mock server plays the endpoint; custom models and their key
// are seeded into the throw-away config dir (the HTTP API for that is T5).
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync, writeFileSync } from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { after, before, describe, it } from "node:test";

import { startServer } from "./helpers/server.mjs";

function startMock() {
  const seen = [];
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      const body = chunks.length ? JSON.parse(Buffer.concat(chunks).toString("utf8")) : undefined;
      seen.push({ method: req.method, url: req.url, auth: req.headers.authorization, body });
      if (req.url !== "/v1/chat/completions") {
        res.writeHead(404, { "Content-Type": "application/json" });
        res.end(JSON.stringify({ error: { message: `no route ${req.url}` } }));
        return;
      }
      if (body.stream) {
        res.writeHead(200, { "Content-Type": "text/event-stream" });
        for (const piece of ["Hel", "lo"]) {
          res.write(`data: ${JSON.stringify({ choices: [{ delta: { content: piece } }] })}\n\n`);
        }
        res.end("data: [DONE]\n\n");
        return;
      }
      res.writeHead(200, { "Content-Type": "application/json" });
      res.end(
        JSON.stringify({
          choices: [{ message: { content: `echo:${body.model}` }, finish_reason: "stop" }],
          usage: { prompt_tokens: 3, completion_tokens: 2 },
        }),
      );
    });
  });
  return new Promise((resolve) =>
    server.listen(0, "127.0.0.1", () =>
      resolve({
        port: server.address().port,
        seen,
        close: () => new Promise((r) => server.close(r)),
      }),
    ),
  );
}

describe("custom OpenAI-compatible models", () => {
  let mock;
  let server;
  let configDir;

  before(async () => {
    mock = await startMock();
    configDir = mkdtempSync(path.join(os.tmpdir(), "loomai-routing-"));
    writeFileSync(
      path.join(configDir, "config.json"),
      JSON.stringify({
        version: 1,
        settings: {},
        custom_models: [
          { id: "local-nokey", provider: "openai-compatible", upstream_model: "qwen-7b", base_url: `http://127.0.0.1:${mock.port}/v1` },
          { id: "local-keyed", provider: "openai-compatible", base_url: `http://127.0.0.1:${mock.port}/v1` },
          { id: "lan-keyed", provider: "openai-compatible", base_url: "http://192.0.2.1/v1" },
        ],
      }),
    );
    writeFileSync(
      path.join(configDir, "secrets.json"),
      JSON.stringify({ version: 1, secrets: { "custom:local-keyed": "sk-mock-key", "custom:lan-keyed": "sk-lan-key" } }),
    );
    mkdirSync(configDir, { recursive: true });
    server = await startServer({ configDir });
  });

  after(async () => {
    await server.stop();
    await mock.close();
    rmSync(configDir, { recursive: true, force: true });
  });

  it("sends /ask to the custom endpoint with the upstream model name and no Authorization header", async () => {
    mock.seen.length = 0;
    const res = await server.post("/ask", { prompt: "hi", model: "local-nokey" });
    assert.equal(res.status, 200, res.text);
    assert.equal(res.json.text, "echo:qwen-7b");
    assert.deepEqual(res.json.usage, { prompt_tokens: 3, completion_tokens: 2 });
    assert.equal(mock.seen.length, 1);
    assert.equal(mock.seen[0].url, "/v1/chat/completions");
    assert.equal(mock.seen[0].auth, undefined);
    assert.equal(mock.seen[0].body.model, "qwen-7b");
    assert.equal(mock.seen[0].body.messages.at(-1).content, "hi");
  });

  it("defaults the upstream model to the custom id and sends the stored key as a Bearer token", async () => {
    mock.seen.length = 0;
    const res = await server.post("/ask", { prompt: "hi", model: "local-keyed", system: "be brief" });
    assert.equal(res.status, 200, res.text);
    assert.equal(res.json.text, "echo:local-keyed");
    assert.equal(mock.seen[0].auth, "Bearer sk-mock-key");
    assert.equal(mock.seen[0].body.messages[0].role, "system");
  });

  it("streams /ask/stream through the same endpoint", async () => {
    mock.seen.length = 0;
    const res = await server.post("/ask/stream", { prompt: "hi", model: "local-nokey" });
    assert.equal(res.status, 200);
    const deltas = [...res.text.matchAll(/^data: (.*)$/gm)].map((m) => m[1]);
    assert.deepEqual(deltas.slice(0, 2).map((d) => JSON.parse(d).delta), ["Hel", "lo"]);
    assert.equal(deltas.at(-1), "[DONE]");
    assert.equal(mock.seen[0].body.stream, true);
  });

  it("reports an unreachable endpoint as a normal 502 with the model's label", async () => {
    await mock.close();
    const res = await server.post("/ask", { prompt: "hi", model: "local-nokey" });
    assert.equal(res.status, 502);
    assert.match(res.json.error.message, /^local-nokey: /);
    mock = await startMock(); // restart so `after` can close it; the port differs, which is fine here
  });

  it("refuses to send a key over plain http to a non-loopback host, without any network call", async () => {
    const res = await server.post("/ask", { prompt: "hi", model: "lan-keyed" });
    assert.equal(res.status, 502);
    assert.match(res.json.error.message, /refusing to send an API key over plain http to 192\.0\.2\.1/);
    assert.ok(!res.text.includes("sk-lan-key"), "the key must never appear in a response");
  });

  it("still routes a prefix model that is not custom exactly as before", async () => {
    const res = await server.post("/ask", { prompt: "hi", model: "gpt-4o" });
    assert.equal(res.status, 502);
    assert.match(res.json.error.message, /OPENAI_API_KEY not set/);
    const claude = await server.post("/ask", { prompt: "hi", model: "claude-x" });
    assert.match(claude.json.error.message, /ANTHROPIC_API_KEY/);
  });
});
