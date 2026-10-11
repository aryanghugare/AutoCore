import { useState } from 'react';
import { scenarios, type Scenario } from '../shared/protocol';
import { useSimulation } from './useSimulation';
import { EcuCards } from './components/EcuCards';
import { Telemetry } from './components/Telemetry';
import { CanTable } from './components/CanTable';
import { DiagnosticsPanel } from './components/DiagnosticsPanel';

const labels: Record<Scenario, string> = { normal: 'Normal operation', 'sensor-failure': 'Sensor failure', timeout: 'Communication timeout', recovery: 'Fault & recovery' };
export default function App() {
  const simulation = useSimulation();
  const [selected, setSelected] = useState<Scenario>('normal');
  const { snapshot, connection, busy, error } = simulation;
  const active = snapshot && ['starting', 'running', 'stopping'].includes(snapshot.run.state);
  const connected = connection === 'connected';
  return <div className="app-shell">
    <aside className="sidebar">
      <a className="brand" href="#"><span className="brand-mark">V</span><div>VIRTUAL ECU<small>COMMUNICATION PLATFORM</small></div></a>
      <span className="nav-label">WORKSPACE</span><div className="nav-active"><span aria-hidden="true">▦</span>Simulation console</div>
      <div className="sidebar-note"><span className="eyebrow">SOFTWARE SIMULATION</span><p>Three ECUs. One virtual bus.<br/>Observe communication,<br/>faults, and recovery.</p></div>
      <div className="sidebar-bottom"><span className={`connection-dot ${connection}`}/><div>LOCAL CONNECTION<small>{connected ? 'HTTP adapter connected' : connection === 'connecting' ? 'Connecting to adapter…' : 'Adapter unavailable'}</small></div></div>
    </aside>
    <main>
      <header className="topbar"><div className="breadcrumb">Workspace <span>/</span><strong>Simulation console</strong></div><span className="version">C++17 ENGINE <span>PROTOCOL v1</span></span></header>
      <div className="workspace">
        <div className="page-heading"><div><span className="eyebrow">COMMUNICATION & DIAGNOSTICS</span><h1>Simulation console</h1><p>Live ECU telemetry and CAN-inspired message traffic.</p></div>
          <div className={`connection-state ${connection}`} role="status"><i/>{connection === 'connecting' ? 'CONNECTING' : connected ? 'CONNECTED' : 'DISCONNECTED'}</div></div>
        {error && <div className="error-banner" role="alert"><strong>{connected ? 'Simulation request error' : 'Connection lost'}</strong><span>{error}</span><small>Telemetry is not presented as current until valid data is available.</small></div>}
        <section className="control-bar" aria-label="Simulation controls">
          <div className="scenario-picker"><label htmlFor="scenario">SCENARIO</label><select id="scenario" value={selected} disabled={!!active || busy} onChange={e => setSelected(e.target.value as Scenario)}>{scenarios.map(s => <option key={s} value={s}>{labels[s]}</option>)}</select></div>
          <div className="run-state"><span className={`connection-dot ${snapshot?.run.state === 'running' ? 'connected' : 'connecting'}`}/>{snapshot?.run.state ?? 'Loading state'}<small>{snapshot?.run.scenario ? labels[snapshot.run.scenario] : 'Ready when connected'}</small></div>
          <div className="control-actions"><button className="button-primary" disabled={!connected || !!active || busy} onClick={() => { void simulation.start(selected); }}>{busy ? 'Working…' : '▶ Start simulation'}</button>
            <button className="button-stop" disabled={!connected || !active || busy} onClick={() => { void simulation.stop(); }}>■ Stop</button></div>
        </section>
        <div className="section-title"><h2>ECU network</h2><span>3 NODES · FIFO DELIVERY</span></div>
        <EcuCards snapshot={snapshot} connection={connection}/>
        <Telemetry snapshot={snapshot} connection={connection}/>
        <div className="data-grid"><CanTable frames={snapshot?.frames ?? []}/><DiagnosticsPanel faults={snapshot?.faults ?? []}/></div>
        {snapshot?.summary && <div className="run-summary" aria-label="Simulation summary"><strong>{snapshot.summary.cancelled ? 'Run stopped by request' : 'Scenario complete'}</strong><span>{snapshot.summary.transmissions} CAN transmissions</span><span>{snapshot.summary.processed} frame copies processed</span><span>{snapshot.summary.dropped} dropped</span><span>{snapshot.summary.active} active faults</span></div>}
        <footer className="workspace-footer"><span>Relative simulation time · scenario pacing 0.25×</span><span>Software CAN simulation · no hardware required</span></footer>
      </div>
    </main>
  </div>;
}
