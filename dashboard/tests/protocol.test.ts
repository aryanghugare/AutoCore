import { describe, expect, it } from 'vitest';
import { isBridgeEvent, isStartCommand, isStopCommand, parseSnapshot } from '../shared/protocol';
import { snapshot, status } from './fixtures';

const can = { version: 1, type: 'can', timeMs: 0, source: 'Engine', id: 2047, payload: [0, 255], routed: 2, dropped: 0 };
describe('wire schema', () => {
  it('accepts boundary IDs, eight bytes, zero and unknown telemetry represented by null', () => {
    expect(isBridgeEvent(can)).toBe(true);
    expect(isBridgeEvent({ ...can, id: 0, payload: Array(8).fill(255) })).toBe(true);
    expect(isBridgeEvent({ ...status, rpm: 0, speedKph: 0 })).toBe(true);
    expect(isBridgeEvent({ ...status, rpm: null, speedKph: null, brakeApplied: null })).toBe(true);
    expect(parseSnapshot(snapshot()).status?.rpm).toBe(1500);
  });
  it.each([{ id: -1 }, { id: 2048 }, { id: 1.5 }, { payload: Array(9).fill(0) }, { payload: [256] },
    { payload: [-1] }, { version: 2 }, { timeMs: -1 }, { source: 'Fake' }, { routed: 5 }])('rejects invalid frames %j', patch => {
    expect(isBridgeEvent({ ...can, ...patch })).toBe(false);
  });
  it('rejects invalid and unsupported status events', () => {
    expect(isBridgeEvent({ ...status, rpm: 12001 })).toBe(false);
    expect(isBridgeEvent({ ...status, speedKph: NaN })).toBe(false);
    expect(isBridgeEvent({ ...status, ecus: { Engine: 'online' } })).toBe(false);
    expect(isBridgeEvent({ ...status, type: 'other' })).toBe(false);
  });
  it('rejects malformed snapshots and over-capacity history', () => {
    expect(() => parseSnapshot(null)).toThrow('malformed');
    expect(() => parseSnapshot({ ...snapshot(), version: 2 })).toThrow('malformed');
    expect(() => parseSnapshot({ ...snapshot(), frames: Array(201).fill({ ...can, sequence: 1 }) })).toThrow('malformed');
    expect(() => parseSnapshot({ ...snapshot(), actions: Array(33).fill({ version: 1, timeMs: 0, type: 'action', action: 'started' }) })).toThrow('malformed');
  });
  it('permits only the defined scenario and stop commands', () => {
    expect(isStartCommand({ version: 1, scenario: 'recovery' })).toBe(true);
    expect(isStartCommand({ version: 1, scenario: '../recovery' })).toBe(false);
    expect(isStartCommand({ version: 1, scenario: 'normal', rpm: 9000 })).toBe(false);
    expect(isStopCommand({ version: 1 })).toBe(true);
    expect(isStopCommand({ version: 1, engine: 'offline' })).toBe(false);
  });
});
