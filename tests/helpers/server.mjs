// Test harness: starts the real loomai binary on a free port, with a throw-away
// config directory, no API keys and an unreachable Ollama, so tests never
// depend on (or touch) the developer's own setup.
import { spawn } from "node:child_process";
import { existsSync, mkdtempSync, rmSync } from "node:fs";
import http from "node:http";
import net from "node:net";
import os from "node:os";
import path from "node:path";
import { fileURLToPath } from "node:url";

const here = path.dirname(fileURLToPath(import.meta.url));
export const repoRoot = path.resolve(here, "..", "..");

function binaryPath() {
  if (process.env.LOOMAI_BIN) return process.env.LOOMAI_BIN;
  const exe = process.platform === "win32" ? "loomai.exe" : "loomai";
  return path.join(repoRoot, "build", exe);
}

export function freePort() {
  return new Promise((resolve, reject) => {
    const probe = net.createServer();
    probe.once("error", reject);
    probe.listen(0, "127.0.0.1", () => {
      const { port } = probe.address();
      probe.close(() => resolve(port));
    });
  });
}

// Minimal HTTP client. Not fetch(): undici refuses a hand-set Origin header in
// some versions, and these tests need exactly that.
export function request(port, { method = "GET", pathname = "/", headers = {}, body } = {}) {
  return new Promise((resolve, reject) => {
    const payload = body === undefined ? undefined : typeof body === "string" ? body : JSON.stringify(body);
    const req = http.request(
      {
        host: "127.0.0.1",
        port,
        method,
        path: pathname,
        headers: {
          ...(payload !== undefined ? { "Content-Type": "application/json", "Content-Length": Buffer.byteLength(payload) } : {}),
          ...headers,
        },
      },
      (res) => {
        const chunks = [];
        res.on("data", (c) => chunks.push(c));
        res.on("end", () => {
          const text = Buffer.concat(chunks).toString("utf8");
          let json;
          try {
            json = JSON.parse(text);
          } catch {
            json = undefined;
          }
          resolve({ status: res.statusCode, headers: res.headers, text, json });
        });
      },
    );
    req.on("error", reject);
    if (payload !== undefined) req.write(payload);
    req.end();
  });
}

// Starts the server; resolves once /health answers. `env` overrides the
// hermetic defaults, `args` are passed on the command line.
export async function startServer({ env = {}, args = [], port, configDir } = {}) {
  const bin = binaryPath();
  if (!existsSync(bin)) {
    throw new Error(`loomai binary not found at ${bin} (build it first, or set LOOMAI_BIN)`);
  }
  const usedPort = port ?? (await freePort());
  const ownsConfigDir = !configDir;
  const usedConfigDir = configDir ?? mkdtempSync(path.join(os.tmpdir(), "loomai-test-"));

  const child = spawn(bin, args, {
    // Run from somewhere that is NOT the repo, so the tests prove that
    // LOOMAI_DASHBOARD_DIR works and nothing relies on ./dashboard.
    cwd: os.tmpdir(),
    env: {
      ...process.env,
      LOOMAI_PORT: String(usedPort),
      LOOMAI_CONFIG_DIR: usedConfigDir,
      LOOMAI_DASHBOARD_DIR: path.join(repoRoot, "dashboard"),
      LOOMAI_OLLAMA_HOST: "http://127.0.0.1:9", // nothing listens on the discard port
      // Keys saved by a test go into <config dir>/secrets.json, never into the
      // developer's real Windows Credential Manager.
      LOOMAI_SECRET_BACKEND: "file",
      OPENAI_API_KEY: "",
      ANTHROPIC_API_KEY: "",
      GEMINI_API_KEY: "",
      ...env,
    },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let output = "";
  child.stdout.on("data", (c) => (output += c));
  child.stderr.on("data", (c) => (output += c));

  const exited = new Promise((resolve) => child.once("exit", (code) => resolve(code)));

  const deadline = Date.now() + 15000;
  let ready = false;
  while (Date.now() < deadline && child.exitCode === null) {
    try {
      const res = await request(usedPort, { pathname: "/health" });
      if (res.status === 200) {
        ready = true;
        break;
      }
    } catch {
      // not listening yet
    }
    await new Promise((r) => setTimeout(r, 100));
  }
  if (!ready) {
    child.kill();
    throw new Error(`server did not become ready; output:\n${output}`);
  }

  return {
    port: usedPort,
    configDir: usedConfigDir,
    output: () => output,
    get: (pathname, headers) => request(usedPort, { pathname, headers }),
    post: (pathname, body, headers) => request(usedPort, { method: "POST", pathname, body, headers }),
    put: (pathname, body, headers) => request(usedPort, { method: "PUT", pathname, body, headers }),
    del: (pathname, headers) => request(usedPort, { method: "DELETE", pathname, headers }),
    async stop() {
      if (child.exitCode === null) child.kill();
      await exited;
      if (ownsConfigDir) rmSync(usedConfigDir, { recursive: true, force: true });
    },
  };
}

// Runs the binary expecting it to exit by itself (bad arguments, ...).
export function runToExit(args, env = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(binaryPath(), args, {
      cwd: os.tmpdir(),
      env: { ...process.env, LOOMAI_DASHBOARD_DIR: path.join(repoRoot, "dashboard"), ...env },
      stdio: ["ignore", "pipe", "pipe"],
    });
    let output = "";
    child.stdout.on("data", (c) => (output += c));
    child.stderr.on("data", (c) => (output += c));
    const timer = setTimeout(() => {
      child.kill();
      reject(new Error(`binary did not exit; output:\n${output}`));
    }, 10000);
    child.once("exit", (code) => {
      clearTimeout(timer);
      resolve({ code, output });
    });
  });
}
