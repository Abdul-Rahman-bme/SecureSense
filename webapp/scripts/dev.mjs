// Runs the backend (auto-restart on change) and the Vite dev server together.
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..');
const procs = [
  ['server', [process.execPath, ['--watch', 'server/index.js']]],
  ['web', [process.execPath, [path.join('node_modules', 'vite', 'bin', 'vite.js')]]],
].map(([name, [cmd, args]]) => {
  const p = spawn(cmd, args, { cwd: root, stdio: 'inherit' });
  p.on('exit', (code) => {
    console.log(`[dev] ${name} exited (${code}); stopping.`);
    for (const other of procs) if (other !== p) other.kill();
    process.exit(code ?? 0);
  });
  return p;
});
process.on('SIGINT', () => procs.forEach((p) => p.kill()));
console.log('[dev] Open http://localhost:5173');
