// Local MQTT broker for development (aedes, plain TCP, NO TLS).
//   npm run broker
// Defaults to 127.0.0.1:1883 (this PC only). To let the real S3 connect over your
// home network, set BROKER_HOST=0.0.0.0 and BROKER_USERNAME/BROKER_PASSWORD, and use
// MQTT_URI "mqtt://<this-pc-ip>:1883" in secrets.h. Bench use only.
import net from 'node:net';
import { timingSafeEqual } from 'node:crypto';
import { Aedes } from 'aedes';

const host = process.env.BROKER_HOST ?? '127.0.0.1';
const port = Number(process.env.BROKER_PORT ?? 1883);
const user = process.env.BROKER_USERNAME;
const pass = process.env.BROKER_PASSWORD;

if (host !== '127.0.0.1' && host !== 'localhost' && !(user && pass)) {
  console.error('Refusing to listen on the network without BROKER_USERNAME and BROKER_PASSWORD.');
  process.exit(1);
}

const same = (a, b) => {
  const x = Buffer.from(String(a ?? '')), y = Buffer.from(String(b ?? ''));
  return x.length === y.length && timingSafeEqual(x, y);
};

const aedes = await Aedes.createBroker();
if (user && pass) {
  aedes.authenticate = (_client, username, password, cb) => {
    const ok = same(username, user) && same(password?.toString(), pass);
    if (ok) return cb(null, true);
    const err = new Error('Bad username or password');
    err.returnCode = 4;
    cb(err, null);
  };
}
aedes.on('client', (c) => console.log(`[broker] + ${c.id}`));
aedes.on('clientDisconnect', (c) => console.log(`[broker] - ${c.id}`));

net.createServer(aedes.handle).listen(port, host, () => {
  console.log(`[broker] MQTT listening on mqtt://${host}:${port}${user ? ' (auth required)' : ' (no auth, local only)'}`);
});
