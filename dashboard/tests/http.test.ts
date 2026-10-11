import { afterEach, describe, expect, it, vi } from 'vitest';
import { PassThrough } from 'node:stream';
import { fileURLToPath } from 'node:url';
import type { IncomingMessage, ServerResponse } from 'node:http';
import { createHandler } from '../server/http';
import { ApiError, SimulationController } from '../server/controller';
import { snapshot } from './fixtures';
const c = new SimulationController('/unused-in-handler-tests');
afterEach(() => vi.restoreAllMocks());
async function request(method: string, url: string, body?: string, headers: Record<string, string> = { 'content-type': 'application/json' }, target = c) {
  const input = Object.assign(new PassThrough(), { method, url, headers }) as unknown as IncomingMessage;
  let status = 0, output = '', responseHeaders: unknown;
  const response = { writeHead: (code: number, h: unknown) => { status = code; responseHeaders = h; }, end: (data: string) => { output = data; } } as unknown as ServerResponse;
  const complete = createHandler(target)(input, response);
  input.emit('data', Buffer.from(body ?? '')); input.emit('end');
  await complete;
  return { status, body: JSON.parse(output) as Record<string, unknown>, headers: responseHeaders };
}
describe('HTTP handlers (in-memory request/response streams)', () => {
  it('serves actual controller state and disables cached responses', async () => {
    const spy = vi.spyOn(c, 'snapshot').mockReturnValue(snapshot());
    const result = await request('GET', '/api/state');
    expect(result.status).toBe(200); expect(spy).toHaveBeenCalledOnce();
    expect(result.headers).toMatchObject({ 'Cache-Control': 'no-store' });
  });
  it('accepts scenario/stop commands through the controller', async () => {
    const start = vi.spyOn(c, 'start').mockReturnValue(snapshot());
    const stop = vi.spyOn(c, 'stop').mockResolvedValue(snapshot());
    expect((await request('POST', '/api/start', '{"version":1,"scenario":"recovery"}')).status).toBe(202);
    expect(start).toHaveBeenCalledWith('recovery');
    expect((await request('POST', '/api/stop', '{"version":1}')).status).toBe(200);
    expect(stop).toHaveBeenCalledOnce();
  });
  it.each(['{}', '{', '{"version":2,"scenario":"normal"}', '{"version":1,"scenario":"other"}', '{"version":1,"scenario":"normal","rpm":5000}'])('rejects malformed start command %s', async body => {
    const start = vi.spyOn(c, 'start');
    expect((await request('POST', '/api/start', body)).status).toBe(400);
    expect(start).not.toHaveBeenCalled();
  });
  it('rejects ECU mutations, large bodies, wrong content types, origins and routes', async () => {
    expect((await request('POST', '/api/stop', '{"version":1,"Engine":"offline"}')).status).toBe(400);
    expect((await request('POST', '/api/start', ' '.repeat(1025))).status).toBe(413);
    expect((await request('POST', '/api/start', '{}', { 'content-type': 'text/plain' })).status).toBe(415);
    expect((await request('POST', '/api/start', '{}', { origin: 'https://other.example', 'content-type': 'application/json' })).status).toBe(403);
    expect((await request('GET', '/unknown')).status).toBe(404);
  });
  it('preserves a rejected start as a visible conflict', async () => {
    vi.spyOn(c, 'start').mockImplementation(() => { throw new ApiError(409, 'Already running'); });
    const result = await request('POST', '/api/start', '{"version":1,"scenario":"normal"}');
    expect(result.status).toBe(409); expect(result.body.error).toBe('Already running');
  });
  it('integrates HTTP commands and snapshots with a real C++ child', async () => {
    const executable = process.env.VAECU_BRIDGE_PATH ?? fileURLToPath(new URL(`../../build/vaecu_bridge${process.platform === 'win32' ? '.exe' : ''}`, import.meta.url));
    const real = new SimulationController(executable);
    try {
      expect((await request('POST', '/api/start', '{"version":1,"scenario":"normal"}', undefined, real)).status).toBe(202);
      expect((await request('POST', '/api/start', '{"version":1,"scenario":"normal"}', undefined, real)).status).toBe(409);
      const deadline = Date.now() + 5000;
      while (real.snapshot().status === null) {
        if (Date.now() > deadline) throw new Error('C++ status did not arrive');
        await new Promise(resolve => setTimeout(resolve, 10));
      }
      const state = await request('GET', '/api/state', undefined, undefined, real);
      expect(state.status).toBe(200); expect(state.body.status).toMatchObject({ rpm: 1500, speedKph: 60 });
      const stopped = await request('POST', '/api/stop', '{"version":1}', undefined, real);
      expect(stopped.status).toBe(200);
      expect(stopped.body.run).toMatchObject({ state: 'stopped' });
      expect(stopped.body.summary).toMatchObject({ cancelled: true, active: 0 });
    } finally { await real.close(); }
  }, 10000);
});
