export const scenarios = ['normal', 'sensor-failure', 'timeout', 'recovery'] as const;
export type Scenario = typeof scenarios[number];
export type Availability = 'online' | 'offline';
export type Health = 'fresh' | 'stale' | 'missing';
export type RunState = 'idle' | 'starting' | 'running' | 'stopping' | 'stopped' | 'error';

interface EventBase { version: 1; timeMs: number }
export interface ActionEvent extends EventBase {
  type: 'action'; action: 'started' | 'stopped' | 'sensor-failed' | 'sensor-recovered' | 'engine-paused' | 'engine-resumed';
}
export interface CanEvent extends EventBase {
  type: 'can'; source: 'Engine' | 'Brake' | 'Scenario'; id: number;
  payload: number[]; routed: number; dropped: number;
}
export interface StatusEvent extends EventBase {
  type: 'status'; ecus: Record<'Engine' | 'Brake' | 'Dashboard', Availability>;
  engineHealth: Health; brakeHealth: Health; rpm: number | null; speedKph: number | null;
  brakeApplied: boolean | null;
}
export interface DiagnosticEvent extends EventBase {
  type: 'diagnostic'; code: number; name: string; status: 'active' | 'cleared';
}
export interface SummaryEvent extends EventBase {
  type: 'summary'; cancelled: boolean; transmissions: number; routed: number; processed: number;
  dropped: number; raised: number; recovered: number; active: number; lost: number; pendingDiscarded: number;
}
export type BridgeEvent = ActionEvent | CanEvent | StatusEvent | DiagnosticEvent | SummaryEvent;
export interface FaultRecord {
  code: number; name: string; status: 'active' | 'cleared'; firstOccurrenceMs: number;
  lastOccurrenceMs: number; recoveryMs: number | null; occurrences: number;
}
export interface Snapshot {
  version: 1; serverId: string; revision: number;
  run: { id: number; state: RunState; scenario: Scenario | null; error: string | null };
  statusAgeMs: number | null; status: StatusEvent | null;
  frames: (CanEvent & { sequence: number })[]; faults: FaultRecord[];
  actions: ActionEvent[]; summary: SummaryEvent | null;
}
export interface StartCommand { version: 1; scenario: Scenario }
export interface StopCommand { version: 1 }

export const isRecord = (value: unknown): value is Record<string, unknown> =>
  typeof value === 'object' && value !== null && !Array.isArray(value);
const integer = (value: unknown, max = Number.MAX_SAFE_INTEGER): value is number =>
  typeof value === 'number' && Number.isSafeInteger(value) && value >= 0 && value <= max;
const nullableNumber = (value: unknown, max?: number) => value === null || integer(value, max);
export const isScenario = (value: unknown): value is Scenario => scenarios.some(s => s === value);
const oneOf = (value: unknown, options: readonly string[]) => options.includes(value as string);
const health = (value: unknown) => oneOf(value, ['fresh', 'stale', 'missing']);
const fault = (v: Record<string, unknown>) => integer(v.code, 65535) && typeof v.name === 'string' && v.name.length <= 128 && oneOf(v.status, ['active', 'cleared']);

export function isBridgeEvent(value: unknown): value is BridgeEvent {
  if (!isRecord(value) || value.version !== 1 || !integer(value.timeMs)) return false;
  switch (value.type) {
    case 'action': return oneOf(value.action, ['started', 'stopped', 'sensor-failed', 'sensor-recovered', 'engine-paused', 'engine-resumed']);
    case 'can': return oneOf(value.source, ['Engine', 'Brake', 'Scenario']) && integer(value.id, 2047) &&
      Array.isArray(value.payload) && value.payload.length <= 8 && value.payload.every(b => integer(b, 255)) && integer(value.routed, 4) && integer(value.dropped, 4);
    case 'status': return isRecord(value.ecus) && ['Engine', 'Brake', 'Dashboard'].every(k => oneOf((value.ecus as Record<string, unknown>)[k], ['online', 'offline'])) &&
      health(value.engineHealth) && health(value.brakeHealth) && nullableNumber(value.rpm, 9000) && nullableNumber(value.speedKph, 300) &&
      (value.brakeApplied === null || typeof value.brakeApplied === 'boolean');
    case 'diagnostic': return fault(value);
    case 'summary': return typeof value.cancelled === 'boolean' &&
      ['transmissions', 'routed', 'processed', 'dropped', 'raised', 'recovered', 'active', 'lost', 'pendingDiscarded'].every(k => integer(value[k]));
    default: return false;
  }
}

export function parseSnapshot(value: unknown): Snapshot {
  if (!isRecord(value) || value.version !== 1 || typeof value.serverId !== 'string' || !integer(value.revision) ||
    !isRecord(value.run) || !integer(value.run.id) || !oneOf(value.run.state, ['idle', 'starting', 'running', 'stopping', 'stopped', 'error']) ||
    !(value.run.scenario === null || isScenario(value.run.scenario)) || !(value.run.error === null || typeof value.run.error === 'string') ||
    !nullableNumber(value.statusAgeMs) || !(value.status === null || (isBridgeEvent(value.status) && value.status.type === 'status')) ||
    !(value.summary === null || (isBridgeEvent(value.summary) && value.summary.type === 'summary')) ||
    !Array.isArray(value.frames) || value.frames.length > 200 || !value.frames.every(f => isBridgeEvent(f) && f.type === 'can' && isRecord(f) && integer(f.sequence)) ||
    !Array.isArray(value.actions) || value.actions.length > 32 || !value.actions.every(a => isBridgeEvent(a) && a.type === 'action') ||
    !Array.isArray(value.faults) || value.faults.length > 11 || !value.faults.every(f => isRecord(f) && fault(f) &&
      integer(f.firstOccurrenceMs) && integer(f.lastOccurrenceMs) && nullableNumber(f.recoveryMs) && integer(f.occurrences))) {
    throw new Error('Unsupported or malformed simulation response');
  }
  return value as unknown as Snapshot;
}

export function isStartCommand(value: unknown): value is StartCommand {
  return isRecord(value) && Object.keys(value).length === 2 && value.version === 1 && isScenario(value.scenario);
}
export function isStopCommand(value: unknown): value is StopCommand {
  return isRecord(value) && Object.keys(value).length === 1 && value.version === 1;
}
