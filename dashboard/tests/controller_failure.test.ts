import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { EventEmitter } from 'node:events';
import { PassThrough } from 'node:stream';
import { performance } from 'node:perf_hooks';
import { SimulationController } from '../server/controller';
import { status } from './fixtures';

const spawn = vi.hoisted(() => vi.fn());
vi.mock('node:child_process', () => ({ spawn }));

// Faulty process fixture: actual streams, controlled completion, no sockets.
class Child extends EventEmitter {
  stdin = new PassThrough();
  stdout = new PassThrough();
  stderr = new PassThrough();
  private closed = false;
  kill = vi.fn((signal: string) => { this.finish(null, signal); return true; });
  finish(code: number | null = 0, signal: string | null = null) {
    if (this.closed) return;
    this.closed = true;
    this.emit('close', code, signal);
  }
  event(value: unknown) { this.stdout.emit('data', Buffer.from(JSON.stringify(value) + '\n')); }
}
let child: Child, controller: SimulationController;
const summary = { version: 1, type: 'summary', timeMs: 250, cancelled: false,
  transmissions: 0, routed: 0, processed: 0, dropped: 0, raised: 0, recovered: 0, active: 0, lost: 0, pendingDiscarded: 0 };
beforeEach(() => {
  vi.useFakeTimers(); child = new Child(); spawn.mockReset().mockReturnValue(child);
  controller = new SimulationController('/fixture/bridge'); controller.start('normal');
});
afterEach(async () => { child.finish(); await controller.close(); vi.restoreAllMocks(); vi.useRealTimers(); });

describe('process failure and transport boundaries', () => {
  it('bounds natural-completion shutdown if the child never exits', async () => {
    child.event({ ...status, ecus: { Engine: 'offline', Brake: 'offline', Dashboard: 'offline' } });
    child.event(summary);
    expect(child.stdin.writableEnded).toBe(true);
    await vi.advanceTimersByTimeAsync(1999); expect(child.kill).not.toHaveBeenCalled();
    await vi.advanceTimersByTimeAsync(1);
    expect(child.kill).toHaveBeenCalledExactlyOnceWith('SIGKILL');
    expect((await controller.waitForExit()).run).toMatchObject({ state: 'error', error: 'C++ adapter shutdown deadline exceeded' });
  });
  it('accepts batches of valid short lines larger than the single-line capacity', () => {
    const frames = Array.from({ length: 300 }, (_, index) => ({ version: 1, type: 'can', timeMs: index,
      source: 'Engine', id: 256, payload: [220, 5, 60, 0], routed: 2, dropped: 0 }));
    const packet = frames.map(f => JSON.stringify(f) + '\n').join('');
    expect(Buffer.byteLength(packet)).toBeGreaterThan(16384);
    child.stdout.emit('data', Buffer.from(packet));
    const state = controller.snapshot();
    expect(state.run.state).not.toBe('error'); expect(state.frames).toHaveLength(200);
    expect(state.frames[0].sequence).toBe(101); expect(state.frames.at(-1)?.sequence).toBe(300);
  });
  it('preserves the initial malformed-event error through secondary failures', () => {
    child.stdout.emit('data', Buffer.from('{}\n'));
    child.stderr.emit('data', Buffer.from('Secondary cleanup failure'));
    expect(controller.snapshot().run.error).toBe('Malformed C++ event');
  });
  it('measures status age independently of system clock adjustments', () => {
    vi.spyOn(performance, 'now').mockReturnValueOnce(1000).mockReturnValueOnce(1600);
    child.event(status);
    vi.setSystemTime(Date.now() - 60000);
    expect(controller.snapshot().statusAgeMs).toBe(600);
  });
  it.each(['', '\n'])('rejects oversized lines and forcefully closes a stuck child (case %#)', async ending => {
    child.stdout.emit('data', Buffer.from('x'.repeat(16385) + ending));
    expect(controller.snapshot().run.state).toBe('error');
    await vi.advanceTimersByTimeAsync(2000);
    expect(child.kill).toHaveBeenCalledExactlyOnceWith('SIGKILL');
    expect((await controller.waitForExit()).run.state).toBe('error');
  });
  it('accepts a valid line exactly at the byte-capacity boundary', () => {
    child.stdout.emit('data', Buffer.from(JSON.stringify(status).padEnd(16384, ' ') + '\n'));
    expect(controller.snapshot().status).toEqual(status);
  });
  it('handles JSON lines split across reads', () => {
    const line = JSON.stringify(status) + '\n';
    child.stdout.emit('data', Buffer.from(line.slice(0, 15)));
    expect(controller.snapshot().status).toBeNull();
    child.stdout.emit('data', Buffer.from(line.slice(15)));
    expect(controller.snapshot().status).toEqual(status);
  });
  it('preserves a UTF-8 diagnostic name split inside a multibyte character', () => {
    const event = { version: 1, type: 'diagnostic', timeMs: 0, code: 0x1001, name: 'sensor ⚠', status: 'active' };
    const bytes = Buffer.from(JSON.stringify(event) + '\n');
    const split = bytes.indexOf(Buffer.from('⚠')) + 1;
    child.stdout.emit('data', bytes.subarray(0, split));
    child.stdout.emit('data', bytes.subarray(split));
    expect(controller.snapshot().faults[0].name).toBe('sensor ⚠');
  });
  it('shares one deadline across simultaneous stop requests', async () => {
    const first = controller.stop(), second = controller.stop();
    expect(vi.getTimerCount()).toBe(1);
    await vi.advanceTimersByTimeAsync(2000);
    const results = await Promise.all([first, second]);
    expect(results.every(s => s.run.state === 'error')).toBe(true);
    expect(child.kill).toHaveBeenCalledExactlyOnceWith('SIGKILL');
    expect(vi.getTimerCount()).toBe(0);
  });
  it('rejects backwards event time and clears exit timers after termination', async () => {
    child.event(status); child.event({ ...status, timeMs: 249 });
    expect(controller.snapshot().run.error).toBe('C++ event time moved backwards');
    child.finish(1); await controller.waitForExit();
    await vi.advanceTimersByTimeAsync(3000); expect(child.kill).not.toHaveBeenCalled();
  });
});
