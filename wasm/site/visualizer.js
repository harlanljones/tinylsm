// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Client-side LSM-tree visualizer (design section 7). Every frame is drawn from
// the STATS JSON returned by the real engine running inside WebAssembly.
const LEVEL_TARGETS = [4, 4, 4]; // L0 triggers a compaction at four files.
export class Visualizer {
  constructor(canvas, counters, memtableCapacity) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.counters = counters;
    this.memtableCapacity = memtableCapacity;
    this.state = null;
    this.previous = null;
    this.pulses = [];
    this.particles = [];
    this.frames = 0;
    this.resize();
    addEventListener('resize', () => this.resize());
  }
  resize() {
    const ratio = Math.min(devicePixelRatio || 1, 2);
    const rect = this.canvas.getBoundingClientRect();
    this.width = Math.max(320, Math.round(rect.width));
    this.height = Math.max(240, Math.round(rect.height || 380));
    this.canvas.width = this.width * ratio;
    this.canvas.height = this.height * ratio;
    this.ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
  }
  update(stats) {
    if (this.previous) {
      for (const [key, fromLevel] of [['flushes', 0], ['compactions', 1]]) {
        const delta = (stats[key] ?? 0) - (this.previous[key] ?? 0);
        if (delta > 0) {
          this.pulses.push({ label: `${key} +${delta}`, born: performance.now() });
          this.spawnParticles(fromLevel, Math.min(delta, 10));
        }
      }
    }
    this.previous = stats;
    this.state = stats;
    this.canvas.dataset.levels = String(stats.levels.length);
    this.renderCounters();
  }
  spawnParticles(fromLevel, count) {
    if (typeof matchMedia === 'function' && matchMedia('(prefers-reduced-motion: reduce)').matches) return;
    for (let i = 0; i < count; ++i)
      this.particles.push({ level: fromLevel, t: 0, speed: 0.01 + Math.random() * 0.02, lane: Math.random() });
  }
  renderCounters() {
    const stats = this.state;
    if (!stats || !this.counters) return;
    const cacheTotal = stats.cache_hits + stats.cache_misses;
    const ratio = cacheTotal === 0 ? 0 : (100 * stats.cache_hits) / cacheTotal;
    const amplitude = stats.logical_bytes === 0 ? 0 : stats.table_bytes_written / stats.logical_bytes;
    const rows = [
      ['writes', stats.writes.toLocaleString()],
      ['reads', stats.reads.toLocaleString()],
      ['memtable', `${(stats.active_bytes / 1024).toFixed(1)} KiB`],
      ['immutables', String(stats.immutables)],
      ['flushes', String(stats.flushes)],
      ['compactions', String(stats.compactions)],
      ['cache hits', `${ratio.toFixed(0)}%`],
      ['write amp', `${amplitude.toFixed(2)}x`],
      ['SST files', String(stats.levels.reduce((sum, n) => sum + n, 0))],
    ];
    this.counters.replaceChildren(
      ...rows.flatMap(([term, value]) => {
        const dt = document.createElement('dt');
        dt.textContent = term;
        const dd = document.createElement('dd');
        dd.textContent = value;
        return [dt, dd];
      }),
    );
  }
  render() {
    const ctx = this.ctx;
    ctx.clearRect(0, 0, this.width, this.height);
    const pad = 18;
    const memtableHeight = 54;
    this.drawMemtable(pad, pad, this.width - pad * 2, memtableHeight);
    const levelsTop = this._memtableBottom + 34;
    const levels = this.state?.levels ?? [];
    const rows = Math.max(levels.length, 1);
    const gap = 10;
    const rowHeight = Math.max(20, Math.min(44, (this.height - levelsTop - pad - (rows - 1) * gap) / rows));
    for (let level = 0; level < rows; ++level)
      this.drawLevel(level, pad, levelsTop + level * (rowHeight + gap), this.width - pad * 2, rowHeight, levels[level] ?? 0);
    this.drawParticles(levelsTop);
    this.drawPulses();
    // Exposed for automated checks: proof that frames were really painted.
    this.canvas.dataset.frames = String(++this.frames);
    this.canvas.dataset.levels = String(levels.length);
  }
  drawMemtable(x, y, width, height) {
    const ctx = this.ctx;
    const stats = this.state;
    const fill = stats ? Math.min(1, stats.active_bytes / this.memtableCapacity) : 0;
    roundRect(ctx, x, y, width, height, 12);
    ctx.fillStyle = '#101821';
    ctx.fill();
    ctx.strokeStyle = '#22303f';
    ctx.stroke();
    roundRect(ctx, x + 1, y + 1, Math.max(0, (width - 2) * fill), height - 2, 10);
    const gradient = ctx.createLinearGradient(x, y, x + width, y);
    gradient.addColorStop(0, '#2f81f7');
    gradient.addColorStop(1, '#1f9e8f');
    ctx.fillStyle = gradient;
    ctx.fill();
    ctx.font = '600 13px ui-monospace, SFMono-Regular, Menlo, monospace';
    ctx.fillStyle = '#e6edf3';
    ctx.fillText('ACTIVE MEMTABLE (concurrent skip list)', x + 12, y + 21);
    ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, monospace';
    ctx.fillStyle = '#9fb0c0';
    ctx.fillText(`${stats ? (stats.active_bytes / 1024).toFixed(1) : '0.0'} KiB · ${(fill * 100).toFixed(0)}% of ${(this.memtableCapacity / 1024).toFixed(0)} KiB threshold`,
      x + 12, y + 40);
    if (stats && stats.immutables > 0) {
      const label = `${stats.immutables} immutable`;
      ctx.font = '600 12px ui-monospace, SFMono-Regular, Menlo, monospace';
      const textWidth = ctx.measureText(label).width;
      roundRect(ctx, x + width - textWidth - 26, y + 12, textWidth + 14, 22, 6);
      ctx.fillStyle = '#f0b429';
      ctx.fill();
      ctx.fillStyle = '#101821';
      ctx.fillText(label, x + width - textWidth - 19, y + 27);
    }
    this._memtableBottom = y + height;
  }
  drawLevel(level, x, y, width, height, files) {
    const ctx = this.ctx;
    ctx.font = '600 12px ui-monospace, SFMono-Regular, Menlo, monospace';
    ctx.fillStyle = '#9fb0c0';
    ctx.fillText(`L${level}`, x, y + height / 2 + 4);
    const innerX = x + 30;
    const innerWidth = width - 30;
    if (files === 0) {
      ctx.strokeStyle = '#1c2733';
      ctx.setLineDash([4, 4]);
      roundRect(ctx, innerX, y, innerWidth, height, 8);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = '#586c7f';
      ctx.fillText(level === 0 ? 'empty — flushes land here' : 'empty', innerX + 10, y + height / 2 + 4);
      return;
    }
    const slots = Math.max(files, LEVEL_TARGETS[level] ?? 4);
    const gap = 3;
    const blockWidth = Math.max(8, (innerWidth - gap * (slots - 1)) / slots);
    const shade = level === 0 ? '#7f5af0' : level === 1 ? '#2cb1a0' : '#2f81f7';
    for (let i = 0; i < slots; ++i) {
      const bx = innerX + i * (blockWidth + gap);
      roundRect(ctx, bx, y, blockWidth, height, 6);
      if (i < files) {
        ctx.fillStyle = shade;
        ctx.globalAlpha = Math.max(0.35, 0.95 - i * 0.12);
        ctx.fill();
        ctx.globalAlpha = 1;
      } else {
        ctx.fillStyle = '#151c26';
        ctx.fill();
      }
      if (level === 0) {
        // Level 0 ranges may overlap, so each file is drawn as its own sheet.
        ctx.strokeStyle = '#0b1017';
        ctx.beginPath();
        ctx.moveTo(bx + blockWidth / 2, y);
        ctx.lineTo(bx + blockWidth / 2, y + height);
        ctx.stroke();
      }
    }
    ctx.fillStyle = '#c9d6e2';
    ctx.fillText(`${files} file${files === 1 ? '' : 's'}`, innerX + 6, y + height / 2 + 4);
    if (level > 0) {
      ctx.font = '11px ui-monospace, SFMono-Regular, Menlo, monospace';
      ctx.fillStyle = '#586c7f';
      ctx.fillText('non-overlapping', innerX + innerWidth - 110, y + height / 2 + 4);
    }
  }
  drawParticles(levelsTop) {
    if (this.particles.length === 0) return;
    const ctx = this.ctx;
    this.particles = this.particles.filter((particle) => particle.t < 1);
    for (const particle of this.particles) {
      const fromX = 40 + particle.lane * (this.width - 80);
      const toY = levelsTop + particle.level * 30 + 8;
      const y = this._memtableBottom + (toY - this._memtableBottom) * particle.t;
      ctx.beginPath();
      ctx.arc(fromX, y, 2.5, 0, Math.PI * 2);
      ctx.fillStyle = `rgba(47,129,247,${1 - particle.t})`;
      ctx.fill();
      particle.t += particle.speed;
    }
  }
  drawPulses() {
    if (this.pulses.length === 0) return;
    const ctx = this.ctx;
    const now = performance.now();
    this.pulses = this.pulses.filter((pulse) => now - pulse.born < 1600);
    this.pulses.forEach((pulse, index) => {
      const age = (now - pulse.born) / 1600;
      ctx.globalAlpha = 1 - age;
      ctx.font = '600 12px ui-monospace, SFMono-Regular, Menlo, monospace';
      ctx.fillStyle = '#3fb950';
      ctx.fillText(pulse.label, this.width - 130, 26 + index * 16);
      ctx.globalAlpha = 1;
    });
  }
}
function roundRect(ctx, x, y, width, height, radius) {
  const r = Math.max(0, Math.min(radius, height / 2, width / 2));
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.arcTo(x + width, y, x + width, y + height, r);
  ctx.arcTo(x + width, y + height, x, y + height, r);
  ctx.arcTo(x, y + height, x, y, r);
  ctx.arcTo(x, y, x + width, y, r);
  ctx.closePath();
}