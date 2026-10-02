// T4: GET /models and GET /models/info against a mock Ollama. Cloud providers
// are only reachable with a key and live at fixed hostnames, so here they show
// up as suggestions; their parsing is covered by the C++ unit tests.
import assert from "node:assert/strict";
import { mkdtempSync, rmSync, writeFileSync } from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { after, before, describe, it } from "node:test";

import { freePort, startServer } from "./helpers/server.mjs";

function startMockOllama() {
  const state = {
    tags: [
      { name: "mistral:7b", size: 4113301824, modified_at: "2025-09-01T10:00:00Z", details: { family: "llama", parameter_size: "7.2B", quantization_level: "Q4_0" } },
      { name: "llama3.1:8b-q5_K_M", size: 5732987392, details: { family: "llama", parameter_size: "8.0B", quantization_level: "Q5_K_M" } },
    ],
    showCalls: 0,
  };
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      res.setHeader("Content-Type", "application/json");
      if (req.method === "GET" && req.url === "/api/tags") {
        res.end(JSON.stringify({ models: state.tags }));
      } else if (req.method === "POST" && req.url === "/api/show") {
        state.showCalls += 1;
        const { model } = JSON.parse(Buffer.concat(chunks).toString("utf8"));
        if (!state.tags.some((t) => t.name === model)) {
          res.statusCode = 404;
          res.end(JSON.stringify({ error: `model '${model}' not found` }));
          return;
        }
        res.end(
          JSON.stringify({
            details: { family: "llama", parameter_size: "8.0B", quantization_level: "Q5_K_M" },
            model_info: { "general.architecture": "llama", "llama.context_length": 131072 },
            capabilities: ["completion", "tools"],
          }),
        );
      } else {
        res.statusCode = 404;
        res.end("{}");
      }
    });
  });
  return new Promise((resolve) =>
    server.listen(0, "127.0.0.1", () =>
      resolve({ port: server.address().port, state, close: () => new Promise((r) => server.close(r)) }),
    ),
  );
}

describe("GET /models and /models/info", () => {
  let ollama;
  let server;
  let configDir;

  before(async () => {
    ollama = await startMockOllama();
    configDir = mkdtempSync(path.join(os.tmpdir(), "loomai-models-"));
    writeFileSync(
      path.join(configDir, "config.json"),
      JSON.stringify({
        version: 1,
        settings: { default_model: "mistral:7b" },
        custom_models: [
          { id: "my-lmstudio", name: "LM Studio Qwen", provider: "openai-compatible", upstream_model: "qwen-7b", base_url: "http://127.0.0.1:1234/v1" },
          // Same id as a local model: the custom entry must win and the local one disappear.
          { id: "mistral:7b", name: "Mistral (alias)", provider: "ollama" },
          { id: "o4-mini", provider: "openai" }, // cloud provider without a key -> not available
        ],
      }),
    );
    server = await startServer({ configDir, env: { LOOMAI_OLLAMA_HOST: `http://127.0.0.1:${ollama.port}` } });
  });

  after(async () => {
    await server.stop();
    await ollama.close();
    rmSync(configDir, { recursive: true, force: true });
  });

  it("groups local, cloud and custom models and reports the default", async () => {
    const res = await server.get("/models");
    assert.equal(res.status, 200, res.text);
    assert.equal(res.json.default, "mistral:7b");
    assert.deepEqual(res.json.errors, {});
    const byCategory = (c) => res.json.models.filter((m) => m.category === c);

    const local = byCategory("local");
    assert.deepEqual(local.map((m) => m.id), ["llama3.1:8b-q5_K_M"]); // mistral:7b is shadowed by the custom alias
    assert.equal(local[0].provider, "ollama");
    assert.equal(local[0].available, true);
    assert.equal(local[0].details.quantization, "Q5_K_M");
    assert.equal(local[0].details.parameter_size, "8.0B");

    const cloud = byCategory("cloud");
    assert.deepEqual([...new Set(cloud.map((m) => m.provider))].sort(), ["anthropic", "gemini", "openai"]);
    assert.ok(cloud.every((m) => m.available === false && m.details.preset === true), "no keys: only suggestions");

    const custom = byCategory("custom");
    assert.deepEqual(custom.map((m) => m.id).sort(), ["my-lmstudio", "mistral:7b", "o4-mini"].sort());
    const lm = custom.find((m) => m.id === "my-lmstudio");
    assert.equal(lm.name, "LM Studio Qwen");
    assert.equal(lm.available, true);
    assert.equal(lm.details.base_url, "http://127.0.0.1:1234/v1");
    assert.equal(lm.details.upstream_model, "qwen-7b");
    assert.equal(custom.find((m) => m.id === "o4-mini").available, false, "provider key missing");
    assert.ok(!res.text.includes("api_key"), "no key material in the listing");
  });

  it("caches the Ollama list, and ?refresh=1 bypasses the cache", async () => {
    ollama.state.tags.push({ name: "phi3:mini", details: {} });
    const cached = await server.get("/models");
    assert.ok(!cached.json.models.some((m) => m.id === "phi3:mini"), "served from the cache");
    const fresh = await server.get("/models?refresh=1");
    assert.ok(fresh.json.models.some((m) => m.id === "phi3:mini"), "refresh sees the new model");
  });

  it("returns lazy details from Ollama for the hover popup, and caches them", async () => {
    ollama.state.showCalls = 0;
    const first = await server.get("/models/info?id=llama3.1%3A8b-q5_K_M");
    assert.equal(first.status, 200, first.text);
    assert.equal(first.json.provider, "ollama");
    assert.equal(first.json.category, "local");
    assert.equal(first.json.details.context_length, 131072);
    assert.deepEqual(first.json.details.capabilities, ["completion", "tools"]);
    await server.get("/models/info?id=llama3.1%3A8b-q5_K_M");
    assert.equal(ollama.state.showCalls, 1, "the second hover must not hit Ollama again");
  });

  it("answers info for cloud and custom entries from the list, and 404/400 correctly", async () => {
    const cloud = await server.get("/models/info?id=gpt-4o");
    assert.equal(cloud.status, 200);
    assert.equal(cloud.json.provider, "openai");
    assert.equal(cloud.json.details.preset, true);

    const custom = await server.get("/models/info?id=my-lmstudio");
    assert.equal(custom.status, 200);
    assert.equal(custom.json.category, "custom");

    const unknownLocal = await server.get("/models/info?id=does-not-exist%3A1b");
    assert.equal(unknownLocal.status, 404);
    assert.match(unknownLocal.json.error.message, /does not know/);

    const unknownCloud = await server.get("/models/info?id=gpt-nonexistent");
    assert.equal(unknownCloud.status, 404);

    assert.equal((await server.get("/models/info")).status, 400);
    assert.equal((await server.get(`/models/info?id=${"x".repeat(201)}`)).status, 400);
  });

  it("is protected by the origin check like every other route", async () => {
    const res = await server.get("/models", { Origin: "http://evil.example" });
    assert.equal(res.status, 403);
  });
});

describe("GET /models with Ollama down", () => {
  let server;
  before(async () => {
    // Nothing listens on this port: the list must still come back, with the reason in `errors`.
    server = await startServer({ env: { LOOMAI_OLLAMA_HOST: `http://127.0.0.1:${await freePort()}` } });
  });
  after(async () => {
    await server.stop();
  });

  it("still answers 200 with the cloud suggestions and names the failed provider", async () => {
    const res = await server.get("/models");
    assert.equal(res.status, 200);
    assert.match(res.json.errors.ollama, /^ollama: /);
    assert.equal(res.json.models.filter((m) => m.category === "local").length, 0);
    assert.ok(res.json.models.filter((m) => m.category === "cloud").length >= 3);
  });

  it("/models/info for a local model reports 502, not a crash", async () => {
    const res = await server.get("/models/info?id=llama3.1%3A8b");
    assert.equal(res.status, 502);
    assert.match(res.json.error.message, /^ollama: /);
  });
});
