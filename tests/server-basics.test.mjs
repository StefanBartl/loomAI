// T0: port, dashboard directory, origin check, payload limit, argument errors.
import assert from "node:assert/strict";
import { after, before, describe, it } from "node:test";

import { freePort, runToExit, startServer } from "./helpers/server.mjs";

describe("server basics", () => {
  let server;
  before(async () => {
    server = await startServer();
  });
  after(async () => {
    await server.stop();
  });

  it("answers /health on the configured port", async () => {
    const res = await server.get("/health");
    assert.equal(res.status, 200);
    assert.equal(res.json.status, "ready");
  });

  it("serves the dashboard from LOOMAI_DASHBOARD_DIR although the cwd is elsewhere", async () => {
    const res = await server.get("/");
    assert.equal(res.status, 200);
    assert.match(res.text, /<html/i);
  });

  it("allows an Origin that matches its own port", async () => {
    for (const host of ["127.0.0.1", "localhost"]) {
      const res = await server.get("/health", { Origin: `http://${host}:${server.port}` });
      assert.equal(res.status, 200, host);
    }
  });

  it("rejects a foreign Origin, including the old hardcoded port 8080", async () => {
    for (const origin of ["http://evil.example", "http://127.0.0.1:8080", "http://localhost:8080"]) {
      const res = await server.get("/health", { Origin: origin });
      assert.equal(res.status, 403, origin);
      assert.match(res.json.error.message, /cross-origin/);
    }
  });

  it("rejects a request body larger than 1 MiB", async () => {
    const res = await server.post("/ask", { prompt: "x".repeat(1024 * 1024 + 10) });
    assert.equal(res.status, 413);
  });

  it("keeps the ai.nvim contract: /health shape and /ask validation", async () => {
    const health = await server.get("/health");
    assert.equal(health.json.model_router_ready, true);
    assert.deepEqual(Object.keys(health.json.backends).sort(), ["anthropic", "gemini", "ollama", "openai"]);
    const bad = await server.post("/ask", {});
    assert.equal(bad.status, 400);
    assert.match(bad.json.error.message, /prompt/);
  });
});

describe("--port and error handling", () => {
  // startServer() polls `port`; LOOMAI_PORT is overridden to a different value,
  // so the server only answers there if the command-line flag won.
  for (const [label, makeArgs] of [
    ["--port N", (p) => ["--port", String(p)]],
    ["--port=N", (p) => [`--port=${p}`]],
  ]) {
    it(`${label} wins over LOOMAI_PORT`, async () => {
      const port = await freePort();
      const decoy = await freePort();
      const server = await startServer({ port, args: makeArgs(port), env: { LOOMAI_PORT: String(decoy) } });
      try {
        const res = await server.get("/health");
        assert.equal(res.status, 200);
      } finally {
        await server.stop();
      }
    });
  }

  it("exits with 2 and says why on an invalid port", async () => {
    for (const bad of ["0", "70000", "abc", "-1", ""]) {
      const { code, output } = await runToExit(["--port", bad]);
      assert.equal(code, 2, `port '${bad}'`);
      assert.match(output, /invalid port/);
    }
  });

  it("exits with 2 on an unknown argument", async () => {
    const { code, output } = await runToExit(["--bogus"]);
    assert.equal(code, 2);
    assert.match(output, /unknown argument/);
  });

  it("exits with 1 when LOOMAI_DASHBOARD_DIR does not exist", async () => {
    const { code, output } = await runToExit([], { LOOMAI_PORT: String(await freePort()), LOOMAI_DASHBOARD_DIR: "Z:/does/not/exist" });
    assert.equal(code, 1);
    assert.match(output, /nicht gefunden/);
  });
});
