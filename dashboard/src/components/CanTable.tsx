import { useEffect, useRef, useState } from 'react';
import type { Snapshot } from '../../shared/protocol';

export const hex = (value: number, width = 3) => value.toString(16).toUpperCase().padStart(width, '0');
export function CanTable({ frames }: { frames: Snapshot['frames'] }) {
  const container = useRef<HTMLDivElement>(null);
  const [follow, setFollow] = useState(true);
  useEffect(() => { if (follow && container.current) container.current.scrollTop = container.current.scrollHeight; }, [frames, follow]);
  return <section className="panel can-panel" aria-label="CAN messages">
    <div className="panel-heading"><div><span className="eyebrow">VIRTUAL CAN BUS</span><h2>Message traffic <span className="count">{frames.length}</span></h2></div>
      <label className="follow"><input type="checkbox" checked={follow} onChange={e => setFollow(e.target.checked)}/>Auto-scroll</label></div>
    <div className="table-scroll" ref={container} tabIndex={0}>
      <table><thead><tr><th>Time / ms</th><th>Source</th><th>CAN ID</th><th>Payload / hex</th><th>Routed</th><th>Dropped</th></tr></thead>
        <tbody>{frames.map(frame => <tr key={frame.sequence}>
          <td className="mono subtle">{frame.timeMs.toFixed(0).padStart(4, '0')}</td><td><span className={`source ${frame.source.toLowerCase()}`}>{frame.source}</span></td>
          <td className="mono can-id">0x{hex(frame.id)}</td><td className="mono payload">{frame.payload.map(b => hex(b, 2)).join(' ') || '(empty)'}</td>
          <td className="mono subtle">{frame.routed}</td><td className={`mono ${frame.dropped ? 'danger-text' : 'subtle'}`}>{frame.dropped}</td>
        </tr>)}</tbody>
      </table>
      {frames.length === 0 && <div className="empty"><span className="empty-glyph">↔</span><strong>No CAN messages yet</strong><p>Start a scenario to observe ECU communication.</p></div>}
    </div><div className="panel-footer">FIFO routing · 11-bit IDs · up to 8 payload bytes · latest 200 frames</div>
  </section>;
}
