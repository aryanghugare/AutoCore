// @vitest-environment jsdom
import { act, type ReactNode } from 'react';
import { createRoot, type Root } from 'react-dom/client';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { Telemetry } from '../src/components/Telemetry';
import { EcuCards } from '../src/components/EcuCards';
import { CanTable } from '../src/components/CanTable';
import { DiagnosticsPanel } from '../src/components/DiagnosticsPanel';
import { snapshot } from './fixtures';
import type { Snapshot } from '../shared/protocol';
(globalThis as unknown as Record<string, unknown>).IS_REACT_ACT_ENVIRONMENT = true;
let host: HTMLDivElement, root: Root;
beforeEach(() => { host = document.createElement('div'); document.body.append(host); root = createRoot(host); });
afterEach(async () => { await act(async () => root.unmount()); host.remove(); });
async function render(node: ReactNode) { await act(async () => root.render(node)); }
const values = () => [...host.querySelectorAll('.metric-value strong')].map(e => e.textContent);
describe('telemetry availability', () => {
  it('renders real values including zero and identifies current samples', async () => {
    const s = snapshot(); s.status!.rpm = 0; s.status!.speedKph = 0;
    await render(<Telemetry snapshot={s} connection="connected"/>);
    expect(values()).toEqual(['0', '0']); expect(host.textContent).toContain('CURRENT');
    expect(host.textContent).toContain('Observed at 250 ms simulation time');
  });
  it.each(['stale', 'offline', 'stopped', 'error', 'disconnected', 'aged', 'sensor-fault', 'missing'] as const)('never labels %s values as current', async mode => {
    const s = snapshot();
    if (mode === 'stale' || mode === 'missing') s.status!.engineHealth = mode;
    if (mode === 'offline') s.status!.ecus.Engine = 'offline';
    if (mode === 'stopped' || mode === 'error') s.run.state = mode;
    if (mode === 'aged') s.statusAgeMs = 2001;
    if (mode === 'sensor-fault') s.faults = [{ code: 0x1001, name: 'engine speed sensor invalid', status: 'active', firstOccurrenceMs: 250, lastOccurrenceMs: 250, recoveryMs: null, occurrences: 1 }];
    await render(<Telemetry snapshot={s} connection={mode === 'disconnected' ? 'disconnected' : 'connected'}/>);
    expect(values()).toEqual(['—', '—']);
    expect(host.textContent).not.toContain('CURRENT'); expect(host.textContent).toContain('historical');
  });
  it('leaves unknown samples empty instead of fabricating telemetry', async () => {
    await render(<Telemetry snapshot={null} connection="connecting"/>);
    expect(values()).toEqual(['—', '—']); expect(host.textContent).not.toContain('Last received:');
  });
  it('does not mark null metrics as current even with a fresh status', async () => {
    const s = snapshot(); s.status!.rpm = null; s.status!.speedKph = null;
    await render(<Telemetry snapshot={s} connection="connected"/>);
    expect(values()).toEqual(['—', '—']); expect(host.textContent).not.toContain('CURRENT');
  });
  it('returns to current telemetry after actual recovery', async () => {
    const s = snapshot(); s.status!.engineHealth = 'stale';
    await render(<Telemetry snapshot={s} connection="connected"/>);
    expect(values()).toEqual(['—', '—']);
    s.status!.engineHealth = 'fresh';
    await render(<Telemetry snapshot={s} connection="connected"/>);
    expect(values()).toEqual(['60', '1,500']);
  });
});
it('shows all ECU states and marks connection loss as unknown', async () => {
  const s = snapshot(); s.status!.ecus.Engine = 'offline';
  await render(<EcuCards snapshot={s} connection="connected"/>);
  expect([...host.querySelectorAll('.badge')].map(e => e.textContent)).toEqual(['offline', 'online', 'online']);
  await render(<EcuCards snapshot={s} connection="disconnected"/>);
  expect([...host.querySelectorAll('.badge')].map(e => e.textContent)).toEqual(['unknown', 'unknown', 'unknown']);
});
it('marks aged ECU states unknown during shutdown', async () => {
  const s = snapshot(); s.run.state = 'stopping'; s.statusAgeMs = 2001;
  await render(<EcuCards snapshot={s} connection="connected"/>);
  expect([...host.querySelectorAll('.badge')].map(e => e.textContent)).toEqual(['unknown', 'unknown', 'unknown']);
});
it('renders actual CAN bytes, timestamps, IDs and a user-selectable scroll policy', async () => {
  const frames: Snapshot['frames'] = [{ version: 1, type: 'can', source: 'Engine', timeMs: 250, id: 0x100, payload: [220, 5, 60, 0], routed: 2, dropped: 0, sequence: 1 }];
  await render(<CanTable frames={frames}/>);
  expect(host.textContent).toContain('0x100'); expect(host.textContent).toContain('DC 05 3C 00'); expect(host.textContent).toContain('0250');
  const follow = host.querySelector('input')!;
  expect(follow.checked).toBe(true);
  await act(async () => follow.click()); expect(follow.checked).toBe(false);
});
it('shows fault occurrence and real recovery separately', async () => {
  await render(<DiagnosticsPanel faults={[{ code: 0x1104, name: 'engine communication timeout', status: 'cleared', firstOccurrenceMs: 1000, lastOccurrenceMs: 1000, recoveryMs: 1250, occurrences: 1 }]}/>);
  expect(host.textContent).toContain('0x1104'); expect(host.textContent).toContain('RECOVERED');
  expect(host.textContent).toContain('1000 ms'); expect(host.textContent).toContain('1250 ms'); expect(host.textContent).toContain('0 active');
});
