// tinylsm — Copyright (c) 2026 Harlan Jones. MIT License.
// Terminal front end for the WebAssembly REPL (design section 7). The command
// grammar is the native CLI's, so keystrokes here exercise the same parser.
const VERBS = ['PUT', 'GET', 'DEL', 'SCAN', 'SYNC', 'FLUSH', 'COMPACT', 'STATS', 'HELP'];
export const HELP = [
  'PUT "key" "value"   append a value (WAL record + skip-list node)',
  'GET "key"           read through memtable, immutables, then L0..LN',
  'DEL "key"           write a tombstone',
  'SCAN ["key"]        seek and iterate merged keys',
  'SYNC                fdatasync the active WAL segment',
  'FLUSH               rotate the memtable into a Level 0 SST',
  'COMPACT             merge levels and drop obsolete versions',
  'STATS               engine counters as JSON',
].join('\n');

export class Terminal {
  constructor({ screen, form, input, bar, controls, onSubmit, onControl }) {
    this.screen = screen;
    this.form = form;
    this.input = input;
    this.bar = bar;
    this.onSubmit = onSubmit;
    this.onControl = onControl;
    this.history = [];
    this.cursor = 0;
    this.busy = false;
    form.addEventListener('submit', (event) => {
      event.preventDefault();
      this.submit();
    });
    input.addEventListener('keydown', (event) => this.onKey(event));
    for (const control of controls) this.addControl(control);
  }
  addControl({ label, hint, action, value }) {
    const button = document.createElement('button');
    button.type = 'button';
    button.textContent = label;
    button.title = hint ?? label;
    button.addEventListener('click', () => this.run(action, value, button));
    this.bar.append(button);
  }
  print(text, kind = 'out') {
    if (text === '' || text == null) return;
    const row = document.createElement('pre');
    row.className = `line ${kind}`;
    row.textContent = String(text).replace(/\s+$/, '');
    this.screen.append(row);
    this.screen.scrollTop = this.screen.scrollHeight;
  }
  echo(line) {
    this.print(`tinylsm> ${line}`, 'in');
  }
  async onKey(event) {
    if (event.key === 'ArrowUp' || event.key === 'ArrowDown') {
      event.preventDefault();
      if (this.history.length === 0) return;
      this.cursor = event.key === 'ArrowUp'
        ? Math.max(0, this.cursor - 1)
        : Math.min(this.history.length, this.cursor + 1);
      this.input.value = this.history[this.cursor] ?? '';
      return;
    }
    if (event.key === 'Tab') {
      event.preventDefault();
      const [first, ...rest] = this.input.value.split(/\s+/);
      const match = VERBS.find((verb) => verb.startsWith((first ?? '').toUpperCase()));
      if (match) this.input.value = [match, ...rest].join(' ');
    }
  }
  async submit() {
    const line = this.input.value.trim();
    if (line === '') return;
    this.input.value = '';
    this.history.push(line);
    this.cursor = this.history.length;
    await this.run('command', line);
  }
  async run(action, value, button) {
    if (this.busy) return;
    this.busy = true;
    if (button) button.disabled = true;
    this.form.classList.add('busy');
    try {
      await this.onControl(action, value);
    } catch (error) {
      this.print(String(error?.message ?? error), 'err');
    } finally {
      this.busy = false;
      if (button) button.disabled = false;
      this.form.classList.remove('busy');
      this.input.focus();
    }
  }
}
