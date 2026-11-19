// Manage dashboard users.
//   npm run user -- add <username>        (prompts for the password)
//   npm run user -- passwd <username>     (reset password; signs out all sessions)
//   npm run user -- list
import { createInterface } from 'node:readline/promises';
import { config } from '../config.js';
import { openDb } from '../db.js';
import { createSessionStore, createUserStore, validatePasswordPolicy } from '../auth.js';

const [action, username] = process.argv.slice(2);
// Uses the same database as the server: SQLite by default, Postgres if DATABASE_URL is set.
const db = await openDb(config);
console.log(`(${db.describe})`);
const users = createUserStore(db);

async function askPassword() {
  if (process.env.NEW_PASSWORD) return process.env.NEW_PASSWORD; // non-interactive use
  const rl = createInterface({ input: process.stdin, output: process.stdout, terminal: true });
  // Hide typed characters.
  rl._writeToOutput = (s) => { if (s.includes('\n') || s.startsWith('New password')) process.stdout.write(s); };
  const pw = await rl.question('New password (min 10 chars): ');
  rl.close();
  process.stdout.write('\n');
  return pw;
}

try {
  if (action === 'list') {
    for (const u of await users.list()) {
      console.log(`${u.username}\t(created ${new Date(Number(u.created_at)).toISOString()})`);
    }
  } else if ((action === 'add' || action === 'passwd') && username) {
    const existing = await users.find(username);
    if (action === 'add' && existing) throw new Error(`User "${username}" already exists`);
    if (action === 'passwd' && !existing) throw new Error(`No user "${username}"`);
    const pw = await askPassword();
    const problem = validatePasswordPolicy(pw);
    if (problem) throw new Error(problem);
    if (action === 'add') {
      await users.create(username, pw);
      console.log(`Created user "${username}".`);
    } else {
      await users.setPassword(existing.id, pw);
      await createSessionStore(db, config).destroyAllFor(existing.id);
      console.log(`Password changed for "${username}"; existing sessions signed out.`);
    }
  } else {
    console.log('Usage: npm run user -- add <username> | passwd <username> | list');
    process.exitCode = 1;
  }
} catch (err) {
  console.error(`Error: ${err.message}`);
  process.exitCode = 1;
} finally {
  await db.close();
}
