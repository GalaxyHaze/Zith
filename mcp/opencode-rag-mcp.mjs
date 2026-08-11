#!/usr/bin/env node
/**
 * Short stdio MCP launcher for the globally installed opencode-rag plugin.
 *
 * The plugin's own `opencode-rag mcp` command is broken on ESM builds because
 * its error handler calls `require("node:path")`. This wrapper starts the same
 * MCP server directly and also wires SIGINT/SIGTERM/stdin shutdown.
 */
import process from "node:process";
import { createRequire } from "node:module";
import { spawn } from "node:child_process";
import { once } from "node:events";
import { readFileSync, rmSync } from "node:fs";

const require = createRequire(
  "/home/diogo/.nvm/versions/node/v20.20.2/lib/node_modules/opencode-rag-plugin/dist/mcp/server.js",
);
const pluginRoot = "/home/diogo/.nvm/versions/node/v20.20.2/lib/node_modules/opencode-rag-plugin";
const serverModule = require(`${pluginRoot}/dist/mcp/server.js`);

let args = process.argv.slice(2);
const configIndex = args.indexOf("--config");
const configPath = configIndex >= 0 ? args[configIndex + 1] : undefined;
let resolvedConfigPath = configPath;

let localServer;
if (configPath) {
  const config = JSON.parse(readFileSync(configPath, "utf8"));
  if (config?.localEmbedding?.enabled) {
    localServer = spawn(
      config.localEmbedding.python ??
        "/home/diogo/.local/share/uv/tools/memsearch/bin/python",
      [
        config.localEmbedding.script ?? "/home/diogo/Zith-Lang/mcp/local-embed-server.py",
        "--port",
        "0",
        "--listen",
        "127.0.0.1",
      ],
      {
        cwd: process.cwd(),
        stdio: ["ignore", "pipe", "pipe"],
        env: { ...process.env, PYTHONUNBUFFERED: "1" },
      },
    );

    const firstLine = await Promise.race([
      (async () => {
        for await (const chunk of localServer.stdout) {
          const text = chunk.toString();
          const line = text.split("\n").find((candidate) => candidate.includes('"port"'));
          if (line) {
            return line;
          }
        }
        throw new Error("local embedding server exited before reporting its port");
      })(),
      once(localServer, "exit").then(([code, signal]) => {
        throw new Error(`local embedding server exited early (code=${code} signal=${signal})`);
      }),
    ]);

    const port = JSON.parse(firstLine).port;
    config.embedding.provider = config.localEmbedding.provider ?? "openai";
    config.embedding.baseUrl = `http://127.0.0.1:${port}/v1`;
    config.embedding.model = config.localEmbedding.model ?? "gpahal/bge-m3-onnx-int8";
    config.embedding.apiKey = config.localEmbedding.apiKey ?? "local";
    if (!config.embedding.vectorDimension) {
      config.embedding.vectorDimension = config.localEmbedding.vectorDimension ?? 1024;
    }
    const tempConfig = `${process.cwd()}/.opencode/openCodeRag-mcp.local.json`;
    // Ensure the directory exists for the temporary resolved config.
    await import("node:fs/promises").then(({ mkdir }) =>
      mkdir(`${process.cwd()}/.opencode`, { recursive: true }),
    );
    await import("node:fs/promises").then(({ writeFile }) =>
      writeFile(tempConfig, JSON.stringify(config, null, 2)),
    );
    resolvedConfigPath = tempConfig;
    args = args.map((arg) => (arg === configPath ? tempConfig : arg));
  }
}

const instance = await serverModule.createMcpServer({
  configPath: resolvedConfigPath,
  cwd: process.cwd(),
});

let closed = false;
async function shutdown() {
  if (closed) {
    return;
  }
  closed = true;
  await instance.close();
  if (localServer) {
    localServer.kill("SIGTERM");
  }
  if (resolvedConfigPath) {
    const tempConfig = `${process.cwd()}/.opencode/openCodeRag-mcp.local.json`;
    rmSync(tempConfig, { force: true });
  }
  process.exit(0);
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);
process.stdin.on("end", shutdown);
process.stdin.on("close", shutdown);

await new Promise(() => {});
