import { afterEach, expect, it, vi } from 'vitest';
import { requestState } from '../src/api';
import { snapshot } from './fixtures';
afterEach(() => { vi.unstubAllGlobals(); vi.restoreAllMocks(); });
it('sends a validated scenario command and accepts a typed response', async () => {
  const fetch = vi.fn().mockResolvedValue(new Response(JSON.stringify(snapshot())));
  vi.stubGlobal('fetch', fetch);
  expect((await requestState('/api/start', { version: 1, scenario: 'recovery' })).run.id).toBe(1);
  expect(fetch).toHaveBeenCalledWith('/api/start', expect.objectContaining({ method: 'POST',
    body: '{"version":1,"scenario":"recovery"}', headers: { 'Content-Type': 'application/json' }, cache: 'no-store' }));
});
it('reports API failures without synthesizing data', async () => {
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response('{"version":1,"error":"Already running"}', { status: 409 })));
  await expect(requestState()).rejects.toThrow('Already running');
});
it('rejects malformed success responses and network failures', async () => {
  const fetch = vi.fn().mockResolvedValueOnce(new Response('{"rpm":1500}')).mockRejectedValueOnce(new Error('Offline'));
  vi.stubGlobal('fetch', fetch);
  await expect(requestState()).rejects.toThrow('malformed');
  await expect(requestState()).rejects.toThrow('Offline');
});
it('accounts for time spent fetching and decoding a delayed snapshot', async () => {
  vi.spyOn(performance, 'now').mockReturnValueOnce(1000).mockReturnValueOnce(4300);
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue(new Response(JSON.stringify(snapshot()))));
  expect((await requestState()).statusAgeMs).toBe(3300);
});
