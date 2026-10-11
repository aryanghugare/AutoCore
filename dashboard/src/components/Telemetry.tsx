import type { Snapshot } from '../../shared/protocol';
import type { Connection } from '../useSimulation';

export function Telemetry({ snapshot, connection }: { snapshot: Snapshot | null; connection: Connection }) {
  const status = snapshot?.status;
  const sensorFault = snapshot?.faults.some(f => f.status === 'active' && (f.code === 0x1001 || f.code === 0x1002)) ?? false;
  const live = connection === 'connected' && snapshot?.run.state === 'running' &&
    status?.engineHealth === 'fresh' && status.ecus.Engine === 'online' && status.rpm !== null && status.speedKph !== null &&
    snapshot.statusAgeMs !== null && snapshot.statusAgeMs <= 2000 && !sensorFault;
  const reason = connection !== 'connected' ? 'Connection unavailable' : sensorFault ? 'Sensor fault · last valid input retained' :
    snapshot?.run.state === 'stopped' ? 'Simulation stopped' : snapshot?.run.state === 'error' ? 'Simulation error' :
    status?.engineHealth === 'stale' ? 'Engine telemetry stale' : (snapshot?.statusAgeMs != null && snapshot.statusAgeMs > 2000) ? 'Telemetry updates delayed' : status ? 'Waiting for current telemetry' : 'Waiting for an Engine sample';
  return <section className="telemetry-grid" aria-label="Vehicle telemetry">
    {([{ label: 'Vehicle speed', value: status?.speedKph, unit: 'km/h', limit: 300, kind: 'speed' },
      { label: 'Engine speed', value: status?.rpm, unit: 'RPM', limit: 12000, kind: 'rpm' }] as const).map(metric =>
      <article className={`metric-card ${metric.kind}`} key={metric.kind}>
        <div className="metric-heading"><h2>{metric.label}</h2><span className={`badge ${live ? 'online' : 'unknown'}`}>{live ? 'CURRENT' : 'UNAVAILABLE'}</span></div>
        <div className="metric-value"><strong>{live && metric.value != null ? metric.value.toLocaleString('en-US') : '—'}</strong><span>{metric.unit}</span></div>
        <div className="meter" aria-hidden="true"><span style={{ width: `${live && metric.value != null ? metric.value / metric.limit * 100 : 0}%` }}/></div>
        <div className="metric-note">{live ? `Observed at ${status.timeMs.toFixed(0)} ms simulation time` : reason}</div>
        {!live && metric.value != null && <div className="last-value">Last received: {metric.value.toLocaleString('en-US')} {metric.unit} · historical</div>}
      </article>) }
  </section>;
}
