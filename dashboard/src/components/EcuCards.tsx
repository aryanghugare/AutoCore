import type { Snapshot } from '../../shared/protocol';
import type { Connection } from '../useSimulation';

export function EcuCards({ snapshot, connection }: { snapshot: Snapshot | null; connection: Connection }) {
  const unavailable = connection !== 'connected' || snapshot?.run.state === 'error' ||
    (snapshot && ['starting', 'running', 'stopping'].includes(snapshot.run.state) &&
      (snapshot.statusAgeMs === null || snapshot.statusAgeMs > 2000));
  return <section className="ecu-grid" aria-label="ECU status">
    {(['Engine', 'Brake', 'Dashboard'] as const).map((name, index) => {
      const state = unavailable ? 'unknown' : snapshot?.status?.ecus[name] ?? 'not started';
      return <article className="ecu-card" key={name}>
        <div className="ecu-icon" aria-hidden="true"><svg viewBox="0 0 24 24"><rect x="6" y="6" width="12" height="12" rx="2"/><path d="M9 2v4m6-4v4M9 18v4m6-4v4M2 9h4m-4 6h4m12-6h4m-4 6h4"/></svg></div>
        <div><span className="eyebrow">NODE 0{index + 1}</span><h2>{name} ECU</h2></div>
        <span className={`badge ${state}`}><i />{state}</span>
      </article>;
    })}
  </section>;
}
