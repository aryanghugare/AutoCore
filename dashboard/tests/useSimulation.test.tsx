// @vitest-environment jsdom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { snapshot } from './fixtures';
import { requestState } from '../src/api';
import { useSimulation } from '../src/useSimulation';
vi.mock('../src/api', () => ({ requestState: vi.fn() }));
(globalThis as unknown as Record<string, unknown>).IS_REACT_ACT_ENVIRONMENT = true;
let host: HTMLDivElement, root: Root, latest: ReturnType<typeof useSimulation>;
function Probe() { latest = useSimulation(); return <div>{latest.connection}</div>; }
beforeEach(() => {
  vi.useFakeTimers({ toFake: ['Date', 'setTimeout', 'clearTimeout', 'setInterval', 'clearInterval', 'performance'] });
  vi.mocked(requestState).mockReset(); host = document.createElement('div'); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); vi.useRealTimers(); });
it('ages retained status while a poll hangs without overlapping requests', async () => {
  vi.mocked(requestState).mockResolvedValueOnce(snapshot()).mockImplementationOnce(() => new Promise(() => {}));
  await act(async () => root.render(<Probe/>));
  expect(latest.connection).toBe('connected');
  await act(async () => vi.advanceTimersByTimeAsync(2250));
  expect(latest.snapshot!.statusAgeMs).toBeGreaterThan(2000);
  expect(requestState).toHaveBeenCalledTimes(2);
});
it('rejects an older poll response after a newer command response', async () => {
  let finish!: (value: ReturnType<typeof snapshot>) => void;
  vi.mocked(requestState).mockResolvedValueOnce(snapshot()).mockImplementationOnce(() => new Promise(resolve => { finish = resolve; }));
  await act(async () => root.render(<Probe/>));
  await act(async () => vi.advanceTimersByTimeAsync(100));
  const newer = snapshot(); newer.revision = 4; newer.run.state = 'stopped';
  vi.mocked(requestState).mockResolvedValueOnce(newer);
  await act(async () => latest.stop());
  await act(async () => finish(snapshot()));
  expect(latest.snapshot!.revision).toBe(4); expect(latest.snapshot!.run.state).toBe('stopped');
});
it('retries a failed connection and clears the error after a valid response', async () => {
  vi.mocked(requestState).mockRejectedValueOnce(new Error('Disconnected')).mockResolvedValueOnce(snapshot());
  await act(async () => root.render(<Probe/>));
  expect(latest.connection).toBe('disconnected'); expect(latest.error).toBe('Disconnected');
  await act(async () => vi.advanceTimersByTimeAsync(1000));
  expect(latest.connection).toBe('connected'); expect(latest.error).toBeNull();
});
it('ignores an obsolete poll failure after a newer successful command', async () => {
  let fail!: (error: Error) => void;
  vi.mocked(requestState).mockResolvedValueOnce(snapshot()).mockImplementationOnce(() => new Promise((_, reject) => { fail = reject; }));
  await act(async () => root.render(<Probe/>));
  await act(async () => vi.advanceTimersByTimeAsync(100));
  const newer = snapshot(); newer.revision = 4; newer.run.state = 'stopped';
  vi.mocked(requestState).mockResolvedValueOnce(newer);
  await act(async () => latest.stop());
  await act(async () => fail(new Error('Old request failed')));
  expect(latest.connection).toBe('connected'); expect(latest.error).toBeNull();
  expect(latest.snapshot!.run.state).toBe('stopped');
});
it('accepts a higher server revision even when its request started earlier', async () => {
  let finish!: (value: ReturnType<typeof snapshot>) => void;
  const polled = snapshot(); polled.revision = 2;
  vi.mocked(requestState).mockResolvedValueOnce(snapshot())
    .mockImplementationOnce(() => new Promise(resolve => { finish = resolve; }))
    .mockResolvedValueOnce(polled);
  await act(async () => root.render(<Probe/>));
  let stopping!: Promise<void>;
  await act(async () => { stopping = latest.stop(); });
  await act(async () => vi.advanceTimersByTimeAsync(100));
  expect(latest.snapshot!.revision).toBe(2);
  const newer = snapshot(); newer.revision = 4; newer.run.state = 'stopped';
  await act(async () => { finish(newer); await stopping; });
  expect(latest.snapshot!.revision).toBe(4); expect(latest.snapshot!.run.state).toBe('stopped');
});
it('continues aging retained status when the system clock moves backwards', async () => {
  vi.mocked(requestState).mockResolvedValueOnce(snapshot()).mockImplementationOnce(() => new Promise(() => {}));
  await act(async () => root.render(<Probe/>));
  vi.setSystemTime(Date.now() - 60000);
  await act(async () => vi.advanceTimersByTimeAsync(2250));
  expect(latest.snapshot!.statusAgeMs).toBeGreaterThan(2000);
});
