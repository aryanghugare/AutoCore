// @vitest-environment jsdom
import { act } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, expect, it, vi } from 'vitest';
import { snapshot } from './fixtures';
import type { Snapshot } from '../shared/protocol';
import type { Connection } from '../src/useSimulation';
const simulation = vi.hoisted(() => ({ snapshot: null as Snapshot | null, connection: 'connecting' as Connection, busy: false,
  error: null as string | null, start: vi.fn(), stop: vi.fn() }));
vi.mock('../src/useSimulation', () => ({ useSimulation: () => simulation }));
import App from '../src/App';
(globalThis as unknown as Record<string, unknown>).IS_REACT_ACT_ENVIRONMENT = true;
let host: HTMLDivElement, root: Root;
beforeEach(() => {
  simulation.snapshot = null; simulation.connection = 'connecting'; simulation.busy = false; simulation.error = null;
  simulation.start.mockReset(); simulation.stop.mockReset();
  host = document.createElement('div'); document.body.append(host); root = createRoot(host);
});
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });
const render = () => act(async () => root.render(<App/>));
const buttons = () => [...host.querySelectorAll('button')];
it('has an explicit loading state and no invented telemetry before connecting', async () => {
  await render(); expect(host.textContent).toContain('CONNECTING'); expect(host.textContent).toContain('Loading state');
  expect(buttons().every(b => b.disabled)).toBe(true);
  expect([...host.querySelectorAll('.metric-value strong')].map(e => e.textContent)).toEqual(['—', '—']);
});
it('selects a scenario and sends only a start or stop command', async () => {
  simulation.connection = 'connected'; simulation.snapshot = snapshot(); simulation.snapshot.run.state = 'stopped';
  await render(); const select = host.querySelector('select')!;
  await act(async () => { select.value = 'recovery'; select.dispatchEvent(new Event('change', { bubbles: true })); });
  await act(async () => buttons()[0].click()); expect(simulation.start).toHaveBeenCalledWith('recovery');
  simulation.snapshot.run.state = 'running'; await render();
  expect(buttons()[0].disabled).toBe(true); expect(select.disabled).toBe(true);
  await act(async () => buttons()[1].click()); expect(simulation.stop).toHaveBeenCalledOnce();
});
it('reports connection errors and disables controls', async () => {
  simulation.connection = 'disconnected'; simulation.error = 'Adapter unavailable'; simulation.snapshot = snapshot();
  await render(); expect(host.querySelector('[role="alert"]')?.textContent).toContain('Adapter unavailable');
  expect(buttons().every(b => b.disabled)).toBe(true);
  expect(host.textContent).not.toContain('CURRENT');
});
