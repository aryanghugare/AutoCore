import { parseSnapshot, type Scenario, type Snapshot } from '../shared/protocol';

export async function requestState(path = '/api/state', command?: { version: 1; scenario?: Scenario }, signal?: AbortSignal): Promise<Snapshot> {
  const requestedAt = performance.now();
  const timeout = AbortSignal.timeout(3500);
  const response = await fetch(path, {
    method: command ? 'POST' : 'GET',
    headers: command ? { 'Content-Type': 'application/json' } : undefined,
    body: command ? JSON.stringify(command) : undefined,
    signal: signal ? AbortSignal.any([signal, timeout]) : timeout,
    cache: 'no-store',
  });
  const data: unknown = await response.json();
  if (!response.ok) {
    const message = typeof data === 'object' && data !== null && 'error' in data && typeof data.error === 'string' ? data.error : `API request failed (${response.status})`;
    throw new Error(message);
  }
  const snapshot = parseSnapshot(data);
  // The server measures age before sending. Include the request's elapsed
  // time conservatively so delayed responses cannot resurrect stale values.
  if (snapshot.statusAgeMs !== null) snapshot.statusAgeMs += Math.ceil(Math.max(0, performance.now() - requestedAt));
  return snapshot;
}
