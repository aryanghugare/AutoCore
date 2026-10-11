import { createServer, type IncomingMessage, type ServerResponse } from 'node:http';
import { isStartCommand, isStopCommand } from '../shared/protocol.js';
import { ApiError, SimulationController } from './controller.js';

export function readCommand(request: IncomingMessage): Promise<unknown> {
  if (request.headers['content-type']?.split(';')[0].trim() !== 'application/json') throw new ApiError(415, 'Use application/json');
  return new Promise((resolve, reject) => {
    let bytes = 0;
    const chunks: Buffer[] = [];
    request.on('data', (chunk: Buffer) => {
      bytes += chunk.length;
      if (bytes > 1024) { reject(new ApiError(413, 'Command exceeds 1024 bytes')); return; }
      chunks.push(chunk);
    });
    request.on('end', () => {
      try { resolve(JSON.parse(Buffer.concat(chunks).toString('utf8'))); }
      catch { reject(new ApiError(400, 'Malformed JSON command')); }
    });
    request.on('aborted', () => reject(new ApiError(400, 'Request aborted')));
    request.on('error', reject);
  });
}

export function createHandler(controller: SimulationController) {
  return async (request: IncomingMessage, response: ServerResponse): Promise<void> => {
    const reply = (status: number, body: unknown) => {
      response.writeHead(status, { 'Content-Type': 'application/json', 'Cache-Control': 'no-store', 'X-Content-Type-Options': 'nosniff' });
      response.end(JSON.stringify(body));
    };
    try {
      // Browser commands use the Vite same-origin proxy. Reject other browser
      // origins, rather than exposing controls via permissive CORS.
      const origin = request.headers.origin;
      if (origin && origin !== 'http://127.0.0.1:5173' && origin !== 'http://localhost:5173') throw new ApiError(403, 'Origin is not allowed');
      if (request.method === 'GET' && request.url === '/api/state') { reply(200, controller.snapshot()); return; }
      if (request.method === 'POST' && request.url === '/api/start') {
        const command = await readCommand(request);
        if (!isStartCommand(command)) throw new ApiError(400, 'Expected {version:1, scenario:<defined scenario>}');
        reply(202, controller.start(command.scenario)); return;
      }
      if (request.method === 'POST' && request.url === '/api/stop') {
        if (!isStopCommand(await readCommand(request))) throw new ApiError(400, 'Expected {version:1}');
        reply(200, await controller.stop()); return;
      }
      throw new ApiError(404, 'Unknown API route or method');
    } catch (error) {
      reply(error instanceof ApiError ? error.status : 500, { version: 1, error: error instanceof Error ? error.message : 'Request failed' });
    }
  };
}

export function createApiServer(controller: SimulationController) {
  return createServer({ maxHeaderSize: 8192, requestTimeout: 5000, headersTimeout: 5000 }, createHandler(controller));
}
