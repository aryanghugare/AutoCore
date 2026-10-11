import { afterEach, describe, expect, it } from 'vitest';
import { fileURLToPath } from 'node:url';
import { spawn } from 'node:child_process';
import { SimulationController } from '../server/controller';
import { isBridgeEvent, parseSnapshot, type Scenario } from '../shared/protocol';
const executable = process.env.VAECU_BRIDGE_PATH ?? fileURLToPath(new URL(`../../build/vaecu_bridge${process.platform === 'win32' ? '.exe' : ''}`, import.meta.url));
const controllers: SimulationController[] = [];
const controller = () => { const value = new SimulationController(executable); controllers.push(value); return value; };
afterEach(async () => { await Promise.all(controllers.splice(0).map(c => c.close())); });
async function until(predicate: () => boolean) {
  const deadline = Date.now() + 5000;
  while (!predicate()) {
    if (Date.now() > deadline) throw new Error('Expected live C++ event did not arrive');
    await new Promise(resolve => setTimeout(resolve, 10));
  }
}

describe('real C++ event stream (no sockets)', () => {
  it.each<[Scenario, number, number, number]>([
    ['normal', 8, 0, 0], ['sensor-failure', 8, 1, 0], ['timeout', 10, 2, 0], ['recovery', 12, 3, 3],
  ])('runs %s with real telemetry, faults and clean shutdown', async (scenario, frames, raised, recovered) => {
    const c = controller();
    expect(c.start(scenario).run.state).toBe('starting');
    await until(() => { const state = c.snapshot(); return state.frames.length >= 2 && state.status !== null; });
    const live = parseSnapshot(c.snapshot());
    expect(live.run.state).toBe('running');
    expect(live.summary).toBeNull();
    expect(live.status?.ecus.Engine).toBe('online');
    expect(live.status?.rpm).toBe(1500);
    expect(live.status?.speedKph).toBe(60);
    const result = parseSnapshot(await c.waitForExit());
    expect(result.run.state).toBe('stopped');
    expect(result.status?.ecus).toEqual({ Engine: 'offline', Brake: 'offline', Dashboard: 'offline' });
    expect(result.frames).toHaveLength(frames);
    expect(result.summary).toMatchObject({ transmissions: frames, raised, recovered, active: raised - recovered, cancelled: false, dropped: 0, pendingDiscarded: 0 });
    expect(result.frames.every(isBridgeEvent)).toBe(true);
    expect(result.frames.filter(f => f.source === 'Engine')[0].payload).toEqual([220, 5, 60, 0]);
    expect(result.frames.some(f => f.source === 'Brake')).toBe(true);
    expect(result.frames.some(f => f.source === 'Scenario')).toBe(true);
    expect(result.frames.map(f => f.timeMs)).toEqual([...result.frames.map(f => f.timeMs)].sort((a,b) => a-b));
    if (scenario === 'recovery') expect(result.faults.every(f => f.status === 'cleared' && f.recoveryMs !== null)).toBe(true);
  }, 12000);

  it('rejects concurrent starts; cancellation preserves faults and permits a clean new run', async () => {
    const c = controller(); c.start('recovery');
    expect(() => c.start('normal')).toThrow('already running');
    await until(() => c.snapshot().faults.length > 0);
    const result = await c.stop();
    expect(result.run.state).toBe('stopped');
    expect(result.summary?.cancelled).toBe(true);
    expect(result.summary?.timeMs).toBeLessThan(1500);
    expect(result.faults.some(f => f.status === 'active')).toBe(true);
    expect(result.summary?.active).toBe(result.faults.filter(f => f.status === 'active').length);
    const next = c.start('normal');
    expect(next.run.id).toBe(2); expect(next.frames).toEqual([]); expect(next.faults).toEqual([]);
    expect((await c.stop()).run.state).toBe('stopped');
  }, 10000);

  it('reports missing executables and rejects commands after controller close', async () => {
    const c = new SimulationController('/missing/vaecu_bridge'); controllers.push(c);
    c.start('normal');
    const result = await c.waitForExit();
    expect(result.run.state).toBe('error'); expect(result.run.error).toContain('Cannot start');
    await c.close(); expect(() => c.start('normal')).toThrow('shutting down');
  });
  it('reports synchronous spawn failures without leaving a phantom active run', async () => {
    const c = new SimulationController(''); controllers.push(c);
    expect(() => c.start('normal')).not.toThrow();
    const result = await c.waitForExit();
    expect(result.run.state).toBe('error'); expect(result.run.error).toContain('Cannot start C++ adapter');
    expect((await c.stop()).run.state).toBe('error');
  });

  it.each(['rpm=5000\n', 'x'.repeat(4096) + '\n'])('rejects unsupported or oversized stdin commands without exposing ECU mutation (case %#)', async command => {
    const child = spawn(executable, ['--scenario', 'normal']);
    let error = '';
    child.stderr.on('data', chunk => { error += chunk.toString(); });
    child.stdout.resume(); child.stdin.end(command);
    const code = await new Promise<number | null>((resolve, reject) => { child.on('error', reject); child.on('close', resolve); });
    expect(code).toBe(1); expect(error).toContain('Unsupported bridge command');
  });
});
