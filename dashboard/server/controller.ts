import { spawn, type ChildProcessWithoutNullStreams } from 'node:child_process';
import { randomUUID } from 'node:crypto';
import { performance } from 'node:perf_hooks';
import { StringDecoder } from 'node:string_decoder';
import { isBridgeEvent, type BridgeEvent, type Snapshot, type Scenario, type FaultRecord } from '../shared/protocol.js';

export class ApiError extends Error {
  constructor(public readonly status: number, message: string) { super(message); }
}

export class SimulationController {
  private child: ChildProcessWithoutNullStreams | null = null;
  private completed: Promise<void> = Promise.resolve();
  private statusAt: number | null = null;
  private closing = false;
  private sequence = 0;
  private lastTime = 0;
  private exitTimer: ReturnType<typeof setTimeout> | null = null;
  private state: Snapshot = {
    version: 1, serverId: randomUUID(), revision: 0,
    run: { id: 0, state: 'idle', scenario: null, error: null },
    statusAgeMs: null, status: null, frames: [], faults: [], actions: [], summary: null,
  };
  constructor(private readonly executable: string) {}

  snapshot(): Snapshot {
    return structuredClone({ ...this.state, statusAgeMs: this.statusAt === null ? null : Math.ceil(Math.max(0, performance.now() - this.statusAt)) });
  }
  private changed() { this.state.revision += 1; }
  private fail(message: string) {
    if (this.state.run.state === 'error') return; // Preserve the first cause.
    this.state.run.state = 'error';
    this.state.run.error = message;
    this.changed();
    const child = this.child;
    if (child && !child.stdin.writableEnded && !child.stdin.destroyed) child.stdin.end();
    this.expectExit();
  }
  private expectExit() {
    const child = this.child;
    if (!child || this.exitTimer) return;
    this.exitTimer = setTimeout(() => {
      this.fail('C++ adapter shutdown deadline exceeded');
      child.kill('SIGKILL');
    }, 2000);
  }

  start(scenario: Scenario): Snapshot {
    if (this.closing) throw new ApiError(503, 'Server is shutting down');
    if (this.child) throw new ApiError(409, 'A simulation is already running or stopping');
    this.state = { ...this.state, run: { id: this.state.run.id + 1, state: 'starting', scenario, error: null },
      status: null, frames: [], faults: [], actions: [], summary: null, statusAgeMs: null };
    this.statusAt = null;
    this.sequence = 0;
    this.lastTime = 0;
    this.changed();
    let child: ChildProcessWithoutNullStreams;
    try {
      child = spawn(this.executable, ['--scenario', scenario], { shell: false, stdio: 'pipe' });
    } catch (error) {
      this.completed = Promise.resolve();
      this.fail(`Cannot start C++ adapter: ${error instanceof Error ? error.message : 'Invalid executable configuration'}`);
      return this.snapshot();
    }
    this.child = child;
    let buffered = '', stderr = '', finalized = false;
    const decoder = new StringDecoder('utf8');
    let resolveClosed!: () => void;
    this.completed = new Promise<void>(resolve => { resolveClosed = resolve; });
    child.stdin.on('error', error => { if (!finalized) this.fail(`Control pipe failed: ${error.message}`); });
    child.on('error', error => this.fail(`Cannot start C++ adapter: ${error.message}`));
    child.stderr.on('data', (chunk: Buffer) => {
      stderr = (stderr + chunk.toString('utf8')).slice(-8192);
      // The bridge only writes errors to stderr. Closing input unblocks its
      // reader so exception paths can join before the process exits.
      this.fail(stderr.trim() || 'C++ adapter failed');
    });
    child.stdout.on('data', (chunk: Buffer) => {
      if (this.state.run.state === 'error') return;
      buffered += decoder.write(chunk);
      let newline: number;
      while ((newline = buffered.indexOf('\n')) >= 0) {
        const line = buffered.slice(0, newline);
        buffered = buffered.slice(newline + 1);
        if (Buffer.byteLength(line) > 16384) { this.fail('C++ event buffer limit exceeded'); return; }
        try {
          const event: unknown = JSON.parse(line);
          if (!isBridgeEvent(event)) throw new Error('Malformed C++ event');
          if (this.state.summary) throw new Error('Event after final summary');
          this.accept(event);
          if (event.type === 'summary') {
            if (!child.stdin.writableEnded && !child.stdin.destroyed) child.stdin.end();
            this.expectExit();
          }
        } catch (error) { this.fail(error instanceof Error ? error.message : 'Invalid C++ event'); break; }
      }
      if (Buffer.byteLength(buffered) > 16384) this.fail('C++ event buffer limit exceeded');
    });
    child.on('close', (code, signal) => {
      finalized = true;
      buffered += decoder.end();
      if (this.exitTimer) clearTimeout(this.exitTimer);
      this.exitTimer = null;
      this.child = null;
      if (this.state.run.state !== 'error') {
        if (code !== 0 || signal || !this.state.summary || buffered.trim()) this.fail(stderr.trim() || 'C++ adapter exited without a valid final summary');
        else { this.state.run.state = 'stopped'; this.changed(); }
      }
      resolveClosed();
    });
    return this.snapshot();
  }

  private accept(event: BridgeEvent) {
    if (event.timeMs < this.lastTime) throw new Error('C++ event time moved backwards');
    this.lastTime = event.timeMs;
    switch (event.type) {
      case 'action':
        this.state.actions = [...this.state.actions, event].slice(-32);
        if (event.action === 'started' && this.state.run.state === 'starting') this.state.run.state = 'running';
        if (event.action === 'stopped') this.state.run.state = 'stopping';
        break;
      case 'can': this.state.frames = [...this.state.frames, { ...event, sequence: ++this.sequence }].slice(-200); break;
      case 'status': this.state.status = event; this.statusAt = performance.now(); break;
      case 'diagnostic': {
        const previous = this.state.faults.find(f => f.code === event.code);
        if (!previous && event.status === 'cleared') throw new Error('Recovery without a fault occurrence');
        if (!previous && this.state.faults.length === 11) throw new Error('Diagnostic record limit exceeded');
        const record: FaultRecord = previous ?? { code: event.code, name: event.name, status: 'active',
          firstOccurrenceMs: event.timeMs, lastOccurrenceMs: event.timeMs, recoveryMs: null, occurrences: 1 };
        if (previous && event.status === 'active' && previous.status === 'cleared') {
          record.occurrences += 1; record.lastOccurrenceMs = event.timeMs;
        }
        record.status = event.status;
        if (event.status === 'cleared') record.recoveryMs = event.timeMs;
        if (!previous) this.state.faults.push(record);
        break;
      }
      case 'summary':
        if (!this.state.status || Object.values(this.state.status.ecus).some(s => s !== 'offline')) throw new Error('Summary without clean ECU shutdown');
        if (event.transmissions !== this.sequence || event.active !== this.state.faults.filter(f => f.status === 'active').length) throw new Error('Inconsistent C++ summary');
        this.state.summary = event;
        this.state.run.state = 'stopping';
        break;
    }
    this.changed();
  }

  async stop(): Promise<Snapshot> {
    const child = this.child;
    if (!child) return this.snapshot();
    if (this.state.run.state !== 'error') { this.state.run.state = 'stopping'; this.changed(); }
    if (!child.stdin.writableEnded && !child.stdin.destroyed) child.stdin.end('stop\n');
    // Graceful input cancellation is portable. A stuck process is forcefully
    // terminated only after this explicit timeout and reported as a failure.
    this.expectExit();
    await this.completed;
    return this.snapshot();
  }
  async close(): Promise<void> { this.closing = true; await this.stop(); }
  async waitForExit(): Promise<Snapshot> { await this.completed; return this.snapshot(); }
}
