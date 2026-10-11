import { fileURLToPath } from 'node:url';
import { resolve } from 'node:path';
import { SimulationController } from './controller.js';
import { createApiServer } from './http.js';

const root = fileURLToPath(new URL('../../../', import.meta.url));
const executable = process.env.VAECU_BRIDGE_PATH ?? resolve(root, 'build', process.platform === 'win32' ? 'vaecu_bridge.exe' : 'vaecu_bridge');
const controller = new SimulationController(executable);
const server = createApiServer(controller);
server.on('error', error => { console.error(`HTTP adapter failed: ${error.message}`); process.exitCode = 1; });
server.listen(8080, '127.0.0.1', () => console.log('Virtual ECU API: http://127.0.0.1:8080 (loopback only)'));

let closing = false;
const shutdown = async () => {
  if (closing) return;
  closing = true;
  await controller.close();
  server.close();
  server.closeAllConnections();
};
process.on('SIGINT', () => { void shutdown(); });
process.on('SIGTERM', () => { void shutdown(); });
