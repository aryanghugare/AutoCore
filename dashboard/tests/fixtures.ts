import type { Snapshot, StatusEvent } from '../shared/protocol';

export const status: StatusEvent = { version: 1, type: 'status', timeMs: 250,
  ecus: { Engine: 'online', Brake: 'online', Dashboard: 'online' },
  engineHealth: 'fresh', brakeHealth: 'fresh', rpm: 1500, speedKph: 60, brakeApplied: false };
export function snapshot(): Snapshot {
  return { version: 1, serverId: 'test-server', revision: 1, run: { id: 1, state: 'running', scenario: 'normal', error: null },
    statusAgeMs: 0, status: structuredClone(status), frames: [], faults: [], actions: [], summary: null };
}
