// T5: /settings, /keys and /models/custom through the real binary.
import assert from "node:assert/strict";
import { existsSync, mkdtempSync, readFileSync, rmSync } from "node:fs";
import http from "node:http";
import os from "node:os";
import path from "node:path";
import { after, before, describe, it } from "node:test";

import { startServer } from "./helpers/server.mjs";

const SECRET = "sk-live-secret-do-not-leak-123";

function mockHttp(handler) {
  const hits = [];
  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on("data", (c) => chunks.push(c));
    req.on("end", () => {
      const text = Buffer.concat(chunks).toString("utf8");
      const body = text ? JSON.parse(text) : undefined;
      hits.push({ method: req.method, url: req.url, auth: req.headers.authorization, body });
      handler(req, res, body);
    });
  });
  return new Promise((resolve) =>
    server.listen(0, "127.0.0.1", () => resolve({ port: server.address().port, hits, close: () => new Promise((r) => server.close(r)) })),
  );
}

const ollamaMock = (tag) =>
  mockHttp((req, res) => {
    res.setHeader("Content-Type", "application/json");
    if (req.url === "/api/tags") res.end(JSON.stringify({ models: [{ name: `${tag}:1b`, details: {} }] }));
    else if (req.url === "/api/generate") res.end(JSON.stringify({ response: `from-${tag}`, done: true, done_reason: "stop" }));
    else res.end("{}");
  });

describe("/settings", () => {
  let dir;
  let server;
  before(async () => {
    dir = mkdtempSync(path.join(os.tmpdir(), "loomai-settings-"));
    // An empty LOOMAI_OLLAMA_HOST counts as unset, so ollama_host is not locked here.
    server = await startServer({ configDir: dir, env: { LOOMAI_OLLAMA_HOST: "" } });
  });
  after(async () => {
    await server.stop();
    rmSync(dir, { recursive: true, force: true });
  });

  it("reports every setting with its source and the config directory", async () => {
    const res = await server.get("/settings");
    assert.equal(res.status, 200);
    assert.deepEqual(Object.keys(res.json.settings).sort(), ["default_model", "language", "ollama_host", "theme"]);
    assert.equal(res.json.settings.theme.value, "system");
    assert.equal(res.json.settings.theme.source, "default");
    assert.equal(res.json.settings.theme.locked, false);
    assert.equal(path.resolve(res.json.config_dir), path.resolve(dir));
  });

  it("applies a partial update, persists it, and survives a restart", async () => {
    const put = await server.put("/settings", { theme: "dark", language: "en" });
    assert.equal(put.status, 200, put.text);
    assert.equal(put.json.settings.theme.value, "dark");
    assert.equal(put.json.settings.theme.source, "config");
    assert.equal(put.json.settings.language.value, "en");
    assert.equal(put.json.settings.default_model.source, "default", "untouched keys stay as they were");
    assert.equal(JSON.parse(readFileSync(path.join(dir, "config.json"), "utf8")).settings.theme, "dark");

    await server.stop();
    server = await startServer({ configDir: dir, env: { LOOMAI_OLLAMA_HOST: "" } });
    assert.equal((await server.get("/settings")).json.settings.theme.value, "dark");
  });

  it("null resets a key to its default", async () => {
    const res = await server.put("/settings", { theme: null });
    assert.equal(res.status, 200);
    assert.equal(res.json.settings.theme.value, "system");
    assert.equal(res.json.settings.theme.source, "default");
  });

  it("rejects bad input with 400 and changes nothing", async () => {
    const before = (await server.get("/settings")).text;
    for (const body of [{ theme: "purple" }, { nope: "x" }, { theme: 5 }, { theme: "dark", language: "klingon" }, { ollama_host: "ftp://x" }]) {
      const res = await server.put("/settings", body);
      assert.equal(res.status, 400, JSON.stringify(body));
      assert.ok(res.json.error.message.length > 0);
    }
    assert.equal((await server.put("/settings", "not json")).status, 400);
    assert.equal((await server.put("/settings", [])).status, 400);
    assert.equal((await server.get("/settings")).text, before);
  });

  it("refuses a cross-origin write", async () => {
    const res = await server.put("/settings", { theme: "dark" }, { Origin: "http://evil.example" });
    assert.equal(res.status, 403);
    assert.equal((await server.get("/settings")).json.settings.theme.value, "system");
  });
});

describe("/settings with an environment-locked value", () => {
  let server;
  before(async () => {
    server = await startServer({ env: { LOOMAI_DEFAULT_MODEL: "from-env:7b" } });
  });
  after(async () => {
    await server.stop();
  });

  it("reports the lock and refuses to change it (409)", async () => {
    const get = await server.get("/settings");
    assert.equal(get.json.settings.default_model.value, "from-env:7b");
    assert.equal(get.json.settings.default_model.source, "env");
    assert.equal(get.json.settings.default_model.locked, true);
    assert.equal(get.json.settings.default_model.env, "LOOMAI_DEFAULT_MODEL");
    const put = await server.put("/settings", { default_model: "other" });
    assert.equal(put.status, 409);
    assert.match(put.json.error.message, /LOOMAI_DEFAULT_MODEL/);
    assert.equal((await server.get("/models")).json.default, "from-env:7b");
  });
});

describe("changing ollama_host while the server runs", () => {
  let a;
  let b;
  let server;
  before(async () => {
    a = await ollamaMock("alpha");
    b = await ollamaMock("beta");
    server = await startServer({ env: { LOOMAI_OLLAMA_HOST: "" } }); // not env-locked
  });
  after(async () => {
    await server.stop();
    await a.close();
    await b.close();
  });

  it("takes effect immediately for both the model list and /ask (no pinned keep-alive client)", async () => {
    assert.equal((await server.put("/settings", { ollama_host: `http://127.0.0.1:${a.port}/` })).status, 200);
    assert.equal((await server.get("/settings")).json.settings.ollama_host.value, `http://127.0.0.1:${a.port}`, "normalised");
    assert.deepEqual((await server.get("/models")).json.models.filter((m) => m.category === "local").map((m) => m.id), ["alpha:1b"]);
    assert.equal((await server.post("/ask", { prompt: "hi", model: "alpha:1b" })).json.text, "from-alpha");

    assert.equal((await server.put("/settings", { ollama_host: `http://127.0.0.1:${b.port}` })).status, 200);
    assert.deepEqual((await server.get("/models")).json.models.filter((m) => m.category === "local").map((m) => m.id), ["beta:1b"]);
    const second = await server.post("/ask", { prompt: "hi", model: "beta:1b" });
    assert.equal(second.json.text, "from-beta", "the request must go to the new host, not the first one");
    assert.equal(b.hits.filter((h) => h.url === "/api/generate").length, 1);
  });
});

describe("/keys", () => {
  let dir;
  let server;
  before(async () => {
    dir = mkdtempSync(path.join(os.tmpdir(), "loomai-keys-"));
    server = await startServer({ configDir: dir });
  });
  after(async () => {
    await server.stop();
    rmSync(dir, { recursive: true, force: true });
  });

  it("starts empty and reports presence only", async () => {
    const res = await server.get("/keys");
    assert.equal(res.status, 200);
    assert.equal(res.json.store, "file");
    assert.deepEqual(Object.keys(res.json.keys).sort(), ["ANTHROPIC_API_KEY", "GEMINI_API_KEY", "OPENAI_API_KEY"]);
    assert.deepEqual(res.json.keys.OPENAI_API_KEY, { set: false, source: "none", locked: false });
  });

  it("stores a key without ever echoing it, and the backends then see it", async () => {
    const put = await server.put("/keys/OPENAI_API_KEY", { value: `  ${SECRET}\n` });
    assert.equal(put.status, 200, put.text);
    assert.equal(put.json.keys.OPENAI_API_KEY.source, "store");
    assert.ok(!put.text.includes(SECRET), "the PUT response must not contain the key");
    assert.ok(!(await server.get("/keys")).text.includes(SECRET));
    assert.ok(!(await server.get("/settings")).text.includes(SECRET));
    assert.ok(!(await server.get("/models")).text.includes(SECRET));

    assert.equal((await server.get("/health")).json.backends.openai, true, "the router now treats OpenAI as configured");
    assert.ok(readFileSync(path.join(dir, "secrets.json"), "utf8").includes(SECRET), "file backend: it lives in secrets.json");
    assert.ok(!existsSync(path.join(dir, "config.json")) || !readFileSync(path.join(dir, "config.json"), "utf8").includes(SECRET));
    assert.ok(!server.output().includes(SECRET), "and never in the server log");
  });

  it("deletes the stored key (idempotent)", async () => {
    const del = await server.del("/keys/OPENAI_API_KEY");
    assert.equal(del.status, 200);
    assert.equal(del.json.keys.OPENAI_API_KEY.source, "none");
    assert.equal((await server.del("/keys/OPENAI_API_KEY")).status, 200);
    assert.equal((await server.get("/health")).json.backends.openai, false);
  });

  it("validates names and values", async () => {
    assert.equal((await server.put("/keys/OPENAI_API_KEY", { value: "two words" })).status, 400);
    assert.equal((await server.put("/keys/OPENAI_API_KEY", { value: "" })).status, 400);
    assert.equal((await server.put("/keys/OPENAI_API_KEY", {})).status, 400);
    assert.equal((await server.put("/keys/OPENAI_API_KEY", { value: 5 })).status, 400);
    assert.equal((await server.put("/keys/NOT_A_KEY", { value: "x" })).status, 400);
    assert.equal((await server.put("/keys/custom:foo", { value: "x" })).status, 400, "custom keys go through /models/custom");
    assert.equal((await server.put("/keys/OPENAI_API_KEY", "nope")).status, 400);
    assert.equal((await server.put("/keys/OPENAI_API_KEY", { value: "x" }, { Origin: "http://evil.example" })).status, 403);
    assert.equal((await server.get("/keys")).json.keys.OPENAI_API_KEY.set, false);
  });
});

describe("/keys with an environment-provided key", () => {
  let server;
  before(async () => {
    server = await startServer({ env: { ANTHROPIC_API_KEY: "env-provided-key" } });
  });
  after(async () => {
    await server.stop();
  });

  it("shows it as locked and refuses to overwrite it", async () => {
    const get = await server.get("/keys");
    assert.deepEqual(get.json.keys.ANTHROPIC_API_KEY, { set: true, source: "env", locked: true });
    assert.ok(!get.text.includes("env-provided-key"));
    const put = await server.put("/keys/ANTHROPIC_API_KEY", { value: "other" });
    assert.equal(put.status, 409);
    assert.ok(!put.text.includes("env-provided-key"));
    assert.equal((await server.del("/keys/ANTHROPIC_API_KEY")).status, 200, "removing the stored copy is allowed");
    assert.equal((await server.get("/keys")).json.keys.ANTHROPIC_API_KEY.source, "env");
  });
});

describe("/models/custom", () => {
  let openai;
  let dir;
  let server;
  before(async () => {
    openai = await mockHttp((req, res, body) => {
      res.setHeader("Content-Type", "application/json");
      res.end(JSON.stringify({ choices: [{ message: { content: `echo:${body?.model}` }, finish_reason: "stop" }] }));
    });
    dir = mkdtempSync(path.join(os.tmpdir(), "loomai-custom-"));
    server = await startServer({ configDir: dir });
  });
  after(async () => {
    await server.stop();
    await openai.close();
    rmSync(dir, { recursive: true, force: true });
  });

  it("adds a model, lists it, routes to it with the stored key, and keeps the key out of every response", async () => {
    const add = await server.post("/models/custom", {
      id: "my-endpoint",
      name: "My endpoint",
      provider: "openai-compatible",
      base_url: `http://127.0.0.1:${openai.port}/v1`,
      upstream_model: "tiny-1b",
      api_key: SECRET,
    });
    assert.equal(add.status, 201, add.text);
    assert.equal(add.json.id, "my-endpoint");
    assert.equal(add.json.has_key, true);
    assert.ok(!add.text.includes(SECRET));

    const list = await server.get("/models/custom");
    assert.deepEqual(list.json.models.map((m) => m.id), ["my-endpoint"]);
    assert.ok(!list.text.includes(SECRET));

    const all = await server.get("/models");
    assert.equal(all.json.models.find((m) => m.id === "my-endpoint").category, "custom");
    assert.ok(!all.text.includes(SECRET));
    assert.ok(!readFileSync(path.join(dir, "config.json"), "utf8").includes(SECRET), "the key is not in config.json");

    const ask = await server.post("/ask", { prompt: "hi", model: "my-endpoint" });
    assert.equal(ask.status, 200, ask.text);
    assert.equal(ask.json.text, "echo:tiny-1b");
    assert.equal(openai.hits.at(-1).auth, `Bearer ${SECRET}`);
  });

  it("rejects duplicates (409) and invalid models (400) and leaves nothing behind", async () => {
    const dup = await server.post("/models/custom", { id: "my-endpoint", provider: "ollama" });
    assert.equal(dup.status, 409);
    for (const body of [
      {},
      { id: "bad id", provider: "ollama" },
      { id: "x", provider: "nope" },
      { id: "x", provider: "openai-compatible" },
      { id: "x", provider: "openai-compatible", base_url: "ftp://h/v1" },
      { id: "x", provider: "ollama", base_url: "http://h" },
      { id: "x", provider: "ollama", api_key: "k" },
      { id: "x", provider: "ollama", extra: "field" },
    ]) {
      const res = await server.post("/models/custom", body);
      assert.equal(res.status, 400, JSON.stringify(body));
    }
    assert.equal((await server.post("/models/custom", "not json")).status, 400);
    assert.equal((await server.get("/models/custom")).json.models.length, 1);
  });

  it("supports ids with '/' and ':' (URL-encoded) for add and delete", async () => {
    const id = "meta-llama/llama-3.1-8b:free";
    const add = await server.post("/models/custom", { id, provider: "openai-compatible", base_url: `http://127.0.0.1:${openai.port}/v1` });
    assert.equal(add.status, 201, add.text);
    const del = await server.del(`/models/custom/${encodeURIComponent(id)}`);
    assert.equal(del.status, 204, del.text);
    assert.equal((await server.get("/models/custom")).json.models.length, 1);
  });

  it("deletes a model together with its key, after which routing falls back", async () => {
    assert.equal((await server.del("/models/custom/missing")).status, 404);
    assert.equal((await server.del("/models/custom/bad%20id")).status, 400);
    assert.equal((await server.del("/models/custom/my-endpoint")).status, 204);
    assert.equal((await server.get("/models/custom")).json.models.length, 0);
    assert.ok(!readFileSync(path.join(dir, "secrets.json"), "utf8").includes(SECRET), "its stored key is gone too");
    const ask = await server.post("/ask", { prompt: "hi", model: "my-endpoint" });
    assert.equal(ask.status, 502, "no longer custom: it goes to (the unreachable) Ollama");
    assert.match(ask.json.error.message, /^ollama: /);
  });

  it("refuses a cross-origin write", async () => {
    const res = await server.post("/models/custom", { id: "evil", provider: "ollama" }, { Origin: "http://evil.example" });
    assert.equal(res.status, 403);
    assert.equal((await server.get("/models/custom")).json.models.length, 0);
  });
});
