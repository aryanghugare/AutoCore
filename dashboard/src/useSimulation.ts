import { useCallback, useEffect, useRef, useState } from 'react';
import { requestState } from './api';
import type { Scenario, Snapshot } from '../shared/protocol';

export type Connection = 'connecting' | 'connected' | 'disconnected';
export function useSimulation() {
  const [snapshot, setSnapshot] = useState<Snapshot | null>(null);
  const [connection, setConnection] = useState<Connection>('connecting');
  const [connectionError, setConnectionError] = useState<string | null>(null);
  const [commandError, setCommandError] = useState<string | null>(null);
  const [busy, setBusy] = useState(false);
  const requests = useRef(0);
  const accepted = useRef<{ order: number; snapshot: Snapshot | null }>({ order: 0, snapshot: null });
  const commandController = useRef<AbortController | null>(null);
  const [receivedAt, setReceivedAt] = useState(0);
  const [clock, setClock] = useState(() => performance.now());
  const apply = useCallback((next: Snapshot, order: number) => {
    const previous = accepted.current;
    const sameServer = previous.snapshot?.serverId === next.serverId;
    if (sameServer && previous.snapshot!.revision > next.revision) return false;
    if (order < previous.order && (!sameServer || previous.snapshot!.revision === next.revision)) return false;
    // Within a server, its revision determines freshness. Request order only
    // breaks revision ties or distinguishes responses across server restarts.
    accepted.current = { order: Math.max(order, previous.order), snapshot: next };
    setSnapshot(next); setReceivedAt(performance.now());
    return true;
  }, []);
  useEffect(() => {
    const cancellation = new AbortController();
    let timer: ReturnType<typeof setTimeout>;
    const ageTimer = setInterval(() => setClock(performance.now()), 250);
    const poll = async () => {
      let delay = 100;
      const order = ++requests.current;
      try {
        const next = await requestState('/api/state', undefined, cancellation.signal);
        if (cancellation.signal.aborted) return;
        if (apply(next, order)) { setConnection('connected'); setConnectionError(null); }
      } catch (error) {
        if (cancellation.signal.aborted) return;
        if (order >= accepted.current.order) {
          setConnection('disconnected');
          setConnectionError(error instanceof Error ? error.message : 'Connection failed');
          delay = 1000;
        }
      }
      if (!cancellation.signal.aborted) timer = setTimeout(() => { void poll(); }, delay);
    };
    void poll();
    return () => { cancellation.abort(); commandController.current?.abort(); clearTimeout(timer); clearInterval(ageTimer); };
  }, [apply]);

  const command = async (path: '/api/start' | '/api/stop', scenario?: Scenario) => {
    setBusy(true); setCommandError(null);
    const order = ++requests.current;
    const cancellation = new AbortController();
    commandController.current = cancellation;
    try {
      const next = await requestState(path, scenario ? { version: 1, scenario } : { version: 1 }, cancellation.signal);
      if (!cancellation.signal.aborted && apply(next, order)) { setConnection('connected'); setConnectionError(null); }
    }
    catch (error) { if (!cancellation.signal.aborted) setCommandError(error instanceof Error ? error.message : 'Command failed'); }
    finally { if (!cancellation.signal.aborted) setBusy(false); }
  };
  const agedSnapshot = snapshot && { ...snapshot, statusAgeMs: snapshot.statusAgeMs === null ? null : snapshot.statusAgeMs + Math.ceil(Math.max(0, clock - receivedAt)) };
  return { snapshot: agedSnapshot, connection, error: commandError ?? connectionError ?? snapshot?.run.error ?? null, busy,
    start: (scenario: Scenario) => command('/api/start', scenario), stop: () => command('/api/stop') };
}
