import type { FaultRecord } from '../../shared/protocol';
import { hex } from './CanTable';

export function DiagnosticsPanel({ faults }: { faults: FaultRecord[] }) {
  const active = faults.filter(f => f.status === 'active').length;
  return <section className="panel diagnostics-panel" aria-label="Diagnostics">
    <div className="panel-heading"><div><span className="eyebrow">DIAGNOSTIC MONITOR</span><h2>Fault records <span className={`count ${active ? 'warn-count' : ''}`}>{active} active</span></h2></div></div>
    <div className="fault-list">
      {faults.length === 0 ? <div className="empty"><span className="empty-glyph healthy">✓</span><strong>No diagnostic faults recorded</strong><p>Occurrences and recoveries appear here.</p></div> :
        [...faults].sort((a, b) => Number(b.status === 'active') - Number(a.status === 'active')).map(fault =>
          <article className={`fault ${fault.status}`} key={fault.code}>
            <div className="fault-heading"><code>0x{hex(fault.code, 4)}</code><span className={`badge ${fault.status === 'active' ? 'fault-active' : 'online'}`}>{fault.status === 'active' ? 'ACTIVE' : 'RECOVERED'}</span></div>
            <h3>{fault.name}</h3><p>First occurrence <span>{fault.firstOccurrenceMs} ms</span></p>
            <p>Last occurrence <span>{fault.lastOccurrenceMs} ms · #{fault.occurrences}</span></p>
            <p>Recovery <span>{fault.recoveryMs === null ? 'Not cleared' : `${fault.recoveryMs} ms`}</span></p>
          </article>) }
    </div><div className="panel-footer">Project-specific fault IDs · simulation time</div>
  </section>;
}
