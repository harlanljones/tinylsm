// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Interactive benchmark plots (design section 7): latency distributions,
// percentile comparison, throughput, and write amplification. Data comes from
// benchmark.jsonl emitted by bench/main.cpp; competitors.jsonl (same record
// shape plus an "engine" field) overlays other engines.
const SPEC_TARGETS = [
  { metric: 'buffered_put', field: 'p99', limit: 5, unit: 'us', label: 'p99 write latency (buffered WAL)', direction: 'below', where: 'measured here' },
  { metric: 'cached_get', field: 'p99', limit: 2, unit: 'us', label: 'p99 cached point read', direction: 'below', where: 'measured here' },
  { metric: 'cold_get', field: 'p99', limit: 150, unit: 'us', label: 'p99 cold point read', direction: 'below', where: 'page cache dropped per SST' },
  { metric: 'concurrent_write', field: 'ops_per_second', limit: 150000, unit: 'ops/s', label: '4-thread write throughput', direction: 'above', where: 'measured here' },
];
export const PALETTE = ['#2f81f7', '#7f5af0', '#2cb1a0', '#f0b429', '#f85149'];

export class Charts {
  constructor({ target, legends, summary }) {
    this.target = target;
    this.legends = legends;
    this.summary = summary;
    this.records = [];
    this.engines = [];
  }
  async load() {
    this.records = await loadJsonl('benchmark.jsonl');
    const competitors = await loadJsonl('competitors.jsonl').catch(() => []);
    this.engines = [{ name: 'tinylsm', records: this.records }, ...groupByEngine(competitors)];
    this.render();
    addEventListener('resize', () => this.render());
  }
  byMetric(metric, engine = 'tinylsm') {
    const rows = engine === 'tinylsm' ? this.records : (this.engines.find((e) => e.name === engine)?.records ?? []);
    return rows.find((row) => row.metric === metric);
  }
  render() {
    if (this.records.length === 0) {
      this.target.textContent = 'benchmark.jsonl is missing — run: ./build-release/tinylsm_bench 20000 > wasm/site/benchmark.jsonl';
      return;
    }
    this.legends.replaceChildren(...this.engines.map((engine, index) => swatch(engine.name, PALETTE[index % PALETTE.length])));
    this.renderSummary();
    this.renderDistribution();
    this.renderPercentiles();
    this.renderThroughput();
    this.renderAmplification();
  }
  renderSummary() {
    this.summary.replaceChildren(...SPEC_TARGETS.map((target) => {
      const measured = this.byMetric(target.metric)?.[target.field];
      const pass = measured !== undefined && (target.direction === 'below' ? measured <= target.limit : measured >= target.limit);
      const tr = document.createElement('tr');
      const cells = [
        target.label,
        `${target.direction === 'below' ? '<' : '>'} ${target.limit.toLocaleString()} ${target.unit}`,
        measured === undefined ? 'not measured' : `${Number(measured).toFixed(target.unit === 'us' ? 3 : 0)} ${target.unit}`,
        measured === undefined ? '—' : pass ? 'pass' : 'miss',
        target.where,
      ];
      for (const [index, text] of cells.entries()) {
        const cell = document.createElement('td');
        cell.textContent = text;
        if (index === 3 && measured !== undefined) cell.className = pass ? 'pass' : 'miss';
        tr.append(cell);
      }
      return tr;
    }));
  }
  renderDistribution() {
    const plot = this.canvas('distribution', 320);
    if (!plot) return;
    const series = [];
    for (const metric of ['buffered_put', 'cached_get', 'cold_get']) {
      const record = this.byMetric(metric);
      if (record?.histogram) series.push({ label: metric, histogram: record.histogram, color: PALETTE[series.length] });
    }
    if (series.length === 0) return drawEmpty(plot.ctx, plot, 'no histograms in benchmark.jsonl');
    const edges = series.flatMap((s) => s.histogram.edges_us);
    const min = Math.min(...edges);
    const max = Math.max(...edges);
    const peak = Math.max(...series.flatMap((s) => s.histogram.counts));
    drawAxes(plot.ctx, plot, {
      xLabels: [formatLatency(min), formatLatency(Math.sqrt(min * max)), formatLatency(max)],
      yLabels: ['0%', '50%', 'peak'],
      title: 'latency distribution (log scale)',
    });
    series.forEach((s, index) => {
      const ctx = plot.ctx;
      ctx.beginPath();
      ctx.strokeStyle = s.color;
      ctx.lineWidth = 2;
      s.histogram.counts.forEach((count, bucket) => {
        const x = plot.x + plot.width * logRatio(s.histogram.edges_us[bucket], min, max);
        const y = plot.y + plot.height * (1 - count / peak);
        bucket === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
      });
      ctx.stroke();
      ctx.fillStyle = s.color;
      ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, monospace';
      ctx.fillText(s.label, plot.x + 8, plot.y + 16 + index * 15);
    });
  }
  renderPercentiles() {
    const plot = this.canvas('percentiles', 300);
    if (!plot) return;
    const groups = ['buffered_put', 'cached_get', 'cold_get'].map((metric) => ({
      label: metric,
      bars: this.engines.map((engine, index) => ({
        value: engine.records.find((row) => row.metric === metric)?.p99 ?? 0,
        color: PALETTE[index % PALETTE.length],
        caption: engine.name,
      })),
    })).filter((group) => group.bars.some((bar) => bar.value > 0));
    if (groups.length === 0) return drawEmpty(plot.ctx, plot, 'no percentile data');
    barChart(plot, groups, { title: 'p99 latency (us, lower is better)' });
  }
  renderThroughput() {
    const plot = this.canvas('throughput', 280);
    if (!plot) return;
    const bars = this.engines.map((engine, index) => ({
      value: engine.records.find((row) => row.metric === 'concurrent_write')?.ops_per_second ?? 0,
      color: PALETTE[index % PALETTE.length],
      caption: engine.name,
    })).filter((bar) => bar.value > 0);
    if (bars.length === 0) return drawEmpty(plot.ctx, plot, 'no throughput data');
    barChart(plot, [{ label: '4 concurrent writers', bars }],
      { title: 'write throughput (ops/sec, higher is better)', valueFormat: (v) => `${(v / 1000).toFixed(0)}k` });
  }
  renderAmplification() {
    const plot = this.canvas('amplification', 280);
    if (!plot) return;
    const bars = this.engines.map((engine, index) => ({
      // Live SST footprint over logical bytes: the figure both engines report.
      value: (() => {
        const record = engine.records.find((row) => row.metric === 'sst_write_amplification');
        return record?.live_ratio ?? record?.ratio ?? 0;
      })(),
      color: PALETTE[index % PALETTE.length],
      caption: engine.name,
    })).filter((bar) => bar.value > 0);
    if (bars.length === 0) return drawEmpty(plot.ctx, plot, 'no write-amplification data');
    barChart(plot, [{ label: 'live SST bytes / logical bytes', bars }],
      { title: 'write amplification (lower is better)', valueFormat: (v) => `${v.toFixed(2)}x` });
  }
  canvas(id, height) {
    const element = document.getElementById(id);
    if (!element) return null;
    const ratio = Math.min(devicePixelRatio || 1, 2);
    const width = Math.max(280, Math.round(element.getBoundingClientRect().width || 640));
    element.width = width * ratio;
    element.height = height * ratio;
    element.style.height = `${height}px`;
    const ctx = element.getContext('2d');
    ctx.setTransform(ratio, 0, 0, ratio, 0, 0);
    ctx.clearRect(0, 0, width, height);
    return { ctx, x: 56, y: 26, width: width - 76, height: height - 62 };
  }
}
async function loadJsonl(name) {
  const response = await fetch(name, { cache: 'no-store' });
  if (!response.ok) throw new Error(`${name}: HTTP ${response.status}`);
  return (await response.text()).split('\n').map((line) => line.trim()).filter(Boolean)
    .map((line) => JSON.parse(line));
}
function groupByEngine(records) {
  const engines = new Map();
  for (const record of records) {
    const name = record.engine ?? 'unknown';
    if (!engines.has(name)) engines.set(name, []);
    engines.get(name).push(record);
  }
  return [...engines].map(([name, rows]) => ({ name, records: rows }));
}
function swatch(name, color) {
  const span = document.createElement('span');
  span.className = 'swatch';
  const dot = document.createElement('i');
  dot.style.background = color;
  span.append(dot, document.createTextNode(name));
  return span;
}
function barChart(plot, groups, { title, valueFormat = (v) => v.toFixed(2) }) {
  const ctx = plot.ctx;
  const peak = Math.max(...groups.flatMap((group) => group.bars.map((bar) => bar.value)));
  drawAxes(ctx, plot, {
    title,
    xLabels: groups.map((group) => group.label),
    yLabels: ['0', valueFormat(peak / 2), valueFormat(peak)],
  });
  const groupWidth = plot.width / groups.length;
  groups.forEach((group, gi) => {
    const barWidth = Math.min(64, (groupWidth - 24) / group.bars.length);
    group.bars.forEach((bar, bi) => {
      if (!bar.value) return;
      const height = plot.height * (bar.value / peak);
      const x = plot.x + gi * groupWidth + (groupWidth - barWidth * group.bars.length) / 2 + bi * barWidth;
      const y = plot.y + plot.height - height;
      ctx.fillStyle = bar.color;
      ctx.globalAlpha = 0.92;
      ctx.fillRect(x + 2, y, barWidth - 4, height);
      ctx.globalAlpha = 1;
      ctx.font = '11px ui-monospace, SFMono-Regular, Menlo, monospace';
      ctx.fillStyle = '#c9d6e2';
      const label = valueFormat(bar.value);
      ctx.fillText(label, x + Math.max(2, (barWidth - ctx.measureText(label).width) / 2), y - 5);
      ctx.fillStyle = '#586c7f';
      ctx.fillText(bar.caption, x + 2, plot.y + plot.height + 14);
    });
  });
}
function drawAxes(ctx, plot, { xLabels = [], yLabels = [], title = '' }) {
  ctx.strokeStyle = '#22303f';
  ctx.lineWidth = 1;
  ctx.beginPath();
  ctx.moveTo(plot.x, plot.y);
  ctx.lineTo(plot.x, plot.y + plot.height);
  ctx.lineTo(plot.x + plot.width, plot.y + plot.height);
  ctx.stroke();
  ctx.font = '11px ui-monospace, SFMono-Regular, Menlo, monospace';
  yLabels.forEach((label, index) => {
    const y = plot.y + plot.height * (1 - index / Math.max(1, yLabels.length - 1));
    ctx.fillStyle = '#586c7f';
    ctx.fillText(label, plot.x - 52, y + 4);
    ctx.strokeStyle = '#161f29';
    ctx.beginPath();
    ctx.moveTo(plot.x, y);
    ctx.lineTo(plot.x + plot.width, y);
    ctx.stroke();
  });
  ctx.fillStyle = '#586c7f';
  xLabels.forEach((label, index) => {
    const x = plot.x + plot.width * (xLabels.length === 1 ? 0 : index / (xLabels.length - 1));
    ctx.fillText(label, Math.min(x, plot.x + plot.width - 54), plot.y + plot.height + 16);
  });
  if (title) {
    ctx.fillStyle = '#9fb0c0';
    ctx.font = '600 12px ui-monospace, SFMono-Regular, Menlo, monospace';
    ctx.fillText(title, plot.x, plot.y - 10);
  }
}
function drawEmpty(ctx, plot, message) {
  ctx.fillStyle = '#586c7f';
  ctx.font = '12px ui-monospace, SFMono-Regular, Menlo, monospace';
  ctx.fillText(message, plot.x, plot.y + 24);
}
function formatLatency(us) {
  if (us < 1) return `${(us * 1000).toFixed(0)}ns`;
  if (us < 1000) return `${us.toFixed(us < 10 ? 1 : 0)}us`;
  return `${(us / 1000).toFixed(1)}ms`;
}
function logRatio(value, min, max) {
  if (max <= min) return 0;
  return Math.max(0, Math.min(1, (Math.log10(value) - Math.log10(min)) / (Math.log10(max) - Math.log10(min))));
}