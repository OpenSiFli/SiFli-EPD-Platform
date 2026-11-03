import fs from 'node:fs';
import path from 'node:path';
import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { docsRoot, repositoryRoot, loadCatalog } from './catalog.mjs';
import { prepare } from './prepare.mjs';

const catalog = loadCatalog();
const inputs = [
  ...['content', 'data', 'public', 'index.md'].map((name) => path.join(docsRoot, name)),
  ...catalog.components.flatMap((entry) => [path.join(repositoryRoot, entry.source), path.join(repositoryRoot, path.dirname(entry.source), 'assets')])
];
const directories = new Set(inputs.map((input) => fs.existsSync(input) && fs.statSync(input).isDirectory() ? input : path.dirname(input)));
let child, timer, stopping = false, restarting = false, pending = false;

async function stopChild() {
  if (!child || child.exitCode !== null || child.signalCode !== null) return;
  const exited = once(child, 'exit');
  child.kill('SIGTERM');
  await exited;
}

async function restart() {
  if (stopping) return;
  if (restarting) { pending = true; return; }
  restarting = true;
  try {
    await stopChild();
    prepare();
    child = spawn(process.execPath, [path.join(docsRoot, 'node_modules/vitepress/bin/vitepress.js'), 'dev', '--host', '127.0.0.1', '--port', '4030', '--strictPort'], { cwd: docsRoot, stdio: 'inherit' });
    child.on('error', (error) => console.error(error.message));
  } catch (error) {
    console.error(error.stack || error.message);
  } finally {
    restarting = false;
    if (pending) { pending = false; timer = setTimeout(restart, 150); }
  }
}

const watchers = [...directories].map((directory) => fs.watch(directory, { recursive: directory !== docsRoot }, (_event, name) => {
  if (!name) return;
  const file = path.resolve(directory, String(name));
  if (!inputs.some((input) => file === input || file.startsWith(input + path.sep))) return;
  clearTimeout(timer);
  timer = setTimeout(restart, 150);
}));

for (const signal of ['SIGINT', 'SIGTERM']) process.on(signal, async () => {
  stopping = true;
  clearTimeout(timer);
  watchers.forEach((watcher) => watcher.close());
  await stopChild();
});
await restart();
