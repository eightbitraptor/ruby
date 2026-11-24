// Timeline widget: data-rx-widget="timeline" data-view="pool" | "fork".
//   pool needs data/RactorPool.js; fork needs data/RactorProcess.js and common.js.
(function () {
  "use strict";

  const reduced = matchMedia("(prefers-reduced-motion: reduce)").matches;
  const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
  const esc = (s) => String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]));

  function el(tag, cls, text) {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  }

  const SVG = "http://www.w3.org/2000/svg";
  function svg(tag, attrs = {}, text) {
    const e = document.createElementNS(SVG, tag);
    for (const [k, v] of Object.entries(attrs)) e.setAttribute(k, v);
    if (text != null) e.textContent = text;
    return e;
  }

  // ------------------------------------------------------------- player

  class Player {
    constructor(length, onStep) {
      this.length = length;
      this.onStep = onStep;
      this.i = 0;
      this.playing = false;
      this.speed = 1;
      this.run = 0;
      this.listeners = [];
    }
    get duration() { return (reduced ? 0 : 1000) / this.speed; }
    on(fn) { this.listeners.push(fn); return this; }
    emit() { this.listeners.forEach((f) => f(this)); }
    async go(i, animated) {
      this.i = Math.max(0, Math.min(this.length, i));
      this.emit();
      await this.onStep(this.i, animated && this.duration > 0, this.duration);
    }
    async play() {
      if (this.playing) return;
      if (this.i >= this.length) await this.go(0, false);
      this.playing = true;
      const run = ++this.run;
      this.emit();
      while (this.playing && run === this.run && this.i < this.length) {
        await this.go(this.i + 1, true);
        if (run !== this.run) return;
        await sleep(this.duration * 0.35);
      }
      if (run === this.run) { this.playing = false; this.emit(); }
    }
    pause() { this.playing = false; this.run++; this.emit(); }
    step(d) { this.pause(); return this.go(this.i + d, d > 0); }
    restart() { this.pause(); return this.go(0, false); }
    reset(length) { this.pause(); this.length = length; return this.go(0, false); }
  }

  function buildControls(player, leading) {
    const wrap = el("div", "tl-controlbar");
    const row = el("div", "tl-controls");
    wrap.append(row);
    if (leading) row.append(leading);
    const btn = (label, title, fn, cls = "rx-btn") => {
      const b = el("button", cls, label);
      b.title = title;
      b.type = "button";
      b.addEventListener("click", fn);
      row.append(b);
      return b;
    };
    const restart = btn("\u27F2 Restart", "Back to the start", () => player.restart());
    const back = btn("\u25C0 Back", "One step back", () => player.step(-1));
    const play = btn("\u25B6 Play", "Play or pause", () => (player.playing ? player.pause() : player.play()), "rx-btn primary");
    const step = btn("Step \u25B6", "One step forward", () => player.step(1));
    const speedWrap = el("label", "tl-group");
    speedWrap.append(el("span", "tl-label", "Speed"));
    const speed = el("select", "rx-select");
    for (const v of [0.5, 1, 2, 4]) speed.append(new Option(`${v}\u00D7`, String(v)));
    speed.value = "1";
    speed.addEventListener("change", () => { player.speed = Number(speed.value); });
    speedWrap.append(speed);
    row.append(speedWrap, el("span", "tl-spacer"));
    const pos = el("span", "tl-pos rx-muted");
    row.append(pos);
    const scrub = el("input", "tl-scrub");
    scrub.type = "range";
    scrub.min = "0";
    scrub.max = String(player.length);
    scrub.value = "0";
    scrub.setAttribute("aria-label", "Scrub through the steps");
    scrub.addEventListener("input", () => { const target = Number(scrub.value); player.pause(); player.go(target, false); });
    wrap.append(scrub);
    player.on((p) => {
      play.textContent = p.playing ? "\u275A\u275A Pause" : "\u25B6 Play";
      restart.disabled = p.i === 0 && !p.playing;
      back.disabled = p.i === 0;
      step.disabled = p.i >= p.length;
      scrub.max = String(p.length);
      scrub.value = String(p.i);
      pos.textContent = `step ${p.i} / ${p.length}`;
    });
    player.emit();
    return wrap;
  }

  function setCaption(root, { step, title, kind, text, ruby, note }) {
    root.innerHTML = "";
    const head = el("div", "tl-cap-title");
    if (step != null) head.append(el("span", "tl-cap-step", step));
    head.append(document.createTextNode(title));
    if (kind) head.append(el("span", `rx-badge ${kind}`, kind));
    root.append(head);
    for (const t of [].concat(text || [])) root.append(el("p", null, t));
    if (note) root.append(el("p", "tl-cap-note", note));
    root.append(el("pre", null, ruby || ""));
  }

  function snapshot(node, visual = node) {
    if (!node) return null;
    return { rect: node.getBoundingClientRect(), clone: visual.cloneNode(true) };
  }

  function settle(anim, duration) {
    return Promise.race([anim.finished.catch(() => {}), sleep(duration + 80)]).then(() => anim.cancel());
  }

  function flyBetween(fxLayer, src, dst, duration, { fade = false, hide = true } = {}) {
    if (!src || !dst || duration <= 0) return Promise.resolve();
    const base = fxLayer.getBoundingClientRect();
    const a = src.rect;
    const b = dst.getBoundingClientRect();
    const clone = src.clone;
    clone.classList.remove("tl-hidden", "tl-pop", "working");
    clone.classList.add("tl-flying");
    const w = clone.classList.contains("tl-env") ? b.width : a.width;
    const h = clone.classList.contains("tl-env") ? b.height : a.height;
    clone.style.left = `${a.left + a.width / 2 - w / 2 - base.left}px`;
    clone.style.top = `${a.top + a.height / 2 - h / 2 - base.top}px`;
    clone.style.width = `${w}px`;
    clone.style.height = `${h}px`;
    fxLayer.append(clone);
    if (hide) dst.classList.add("tl-hidden");
    const dx = b.left + b.width / 2 - (a.left + a.width / 2);
    const dy = b.top + b.height / 2 - (a.top + a.height / 2);
    const lift = dy < 0 ? -26 : 26;
    const anim = clone.animate(
      [
        { transform: "translate(0, 0) scale(1)", opacity: 1 },
        { transform: `translate(${dx * 0.5}px, ${dy * 0.5 + lift}px) scale(1.08)`, opacity: 1, offset: 0.5 },
        { transform: `translate(${dx}px, ${dy}px) scale(${fade ? 0.6 : 1})`, opacity: fade ? 0.2 : 1 },
      ],
      { duration, easing: "cubic-bezier(.45, 0, .25, 1)", fill: "forwards" }
    );
    return settle(anim, duration).then(() => {
      clone.remove();
      dst.classList.remove("tl-hidden");
    });
  }

  function sweep(container, duration, label, colorVar) {
    if (duration <= 0) return Promise.resolve();
    const s = el("div", "tl-sweep");
    s.style.setProperty("--sw", colorVar);
    s.style.left = "0";
    s.append(el("span", "tl-sweep-label", label));
    container.append(s);
    const w = container.getBoundingClientRect().width;
    const anim = s.animate(
      [{ transform: "translateX(0)", opacity: 0 }, { opacity: 1, offset: 0.15 }, { opacity: 1, offset: 0.85 }, { transform: `translateX(${w - 70}px)`, opacity: 0 }],
      { duration, easing: "ease-in-out" }
    );
    return settle(anim, duration).then(() => s.remove());
  }

  function pop(node) {
    if (!node) return;
    node.classList.add("tl-pop");
    node.addEventListener("animationend", () => node.classList.remove("tl-pop"), { once: true });
  }

  // --------------------------------------------------------------- pool

  const ACTIONS = { 1: "Dispatch", 2: "Receive", 3: "Process", 4: "Collect", 5: "GlobalCollect", 6: "Stop", 7: "Observe" };
  const HEAP = 32;
  const decode = (token) => ({ action: Math.floor(token / 10), worker: token % 10 });

  const wkInit = () => ({ phase: "idle", mailbox: null, hand: null, ready: null, processed: [], gcs: 0, value: null, closed: false, token: false });
  const poolInitial = () => ({ pending: [1, 2, 3], main: { 1: "pending", 2: "pending", 3: "pending" }, workers: { 1: wkInit(), 2: wkInit() }, completed: [], globalGC: false });

  function poolApply(MODE, prev, token) {
    const s = JSON.parse(JSON.stringify(prev));
    const { action, worker } = decode(token);
    const W = s.workers[worker];
    switch (action) {
      case 1: {
        const j = s.pending.shift();
        s.main[j] = MODE[j] === "move" ? "moved" : MODE[j] === "share" ? "shared" : "original";
        W.mailbox = j;
        W.phase = "queued";
        break;
      }
      case 2: W.hand = W.mailbox; W.mailbox = null; W.phase = "received"; break;
      case 3: W.ready = W.hand; W.processed.push(W.hand); W.hand = null; W.gcs += 1; W.phase = "ready"; break;
      case 4: s.completed.push({ job: W.ready, by: worker }); W.ready = null; W.phase = "idle"; break;
      case 5: s.globalGC = true; break;
      case 6: W.closed = true; W.token = true; W.phase = "closing"; break;
      case 7: W.token = false; W.value = [worker, W.processed.slice(), HEAP * W.processed.length]; W.phase = "observed"; break;
    }
    return s;
  }

  function poolStates(MODE, schedule) {
    const out = [poolInitial()];
    for (const t of schedule) out.push(poolApply(MODE, out[out.length - 1], t));
    return out;
  }

  function holderOf(s, j) {
    for (const w of [1, 2]) {
      const W = s.workers[w];
      if (W.mailbox === j || W.hand === j || W.processed.includes(j)) return w;
    }
    return null;
  }

  function jobCard(MODE, j, where, s) {
    const mode = MODE[j];
    let cls = "";
    let sub = mode;
    if (where === "main") {
      const st = s.main[j];
      const w = holderOf(s, j);
      if (st === "pending") { cls = "pending"; sub = `${mode} \u00B7 waiting in FIFO`; }
      else if (st === "shared") { cls = "shared"; sub = `same object as W${w}'s`; }
      else if (st === "original") { cls = "original"; sub = `original; W${w} got a copy`; }
      else { cls = "ghost"; sub = `moved to W${w}; MovedObject`; }
    } else {
      if (mode === "share") { cls = "shared"; sub = "same object as main's"; }
      else if (mode === "copy") { cls = "copy"; sub = "deep copy of main's"; }
      else { cls = "moved"; sub = "moved here from main"; }
    }
    return `<div class="tl-job ${cls}" data-job="${j}"><b>job ${j}</b><small>${esc(sub)}</small></div>`;
  }

  function portHtml(name, note, envs, closed) {
    return `<div class="tl-port${closed ? " closed" : ""}" data-port="${name}"><span class="tl-port-name">${name}</span>${closed ? '<span class="tl-lock"></span><span class="tl-port-note">closed</span>' : envs.join("") || (note ? `<span class="tl-port-note">${esc(note)}</span>` : "")}</div>`;
  }

  function mainRoom(MODE, s) {
    const done = s.completed.map((c) => `<span class="tl-chip ok" data-job="${c.job}">job ${c.job} \u2713 <small>via R${c.by}</small></span>`).join("");
    const values = [1, 2].filter((w) => s.workers[w].value).map((w) => {
      const [, jobs, n] = s.workers[w].value;
      return `<span class="tl-chip exit" data-w="${w}">W${w}.value \u2192 [${w}, [${jobs.join(", ")}], ${n}]</span>`;
    }).join("");
    const tokens = [1, 2].filter((w) => s.workers[w].token).map((w) => `<span class="tl-env token" data-w="${w}" title="[worker ${w}, :exited]"></span>`);
    const results = [1, 2].map((w) => portHtml(`R${w}`, `results of W${w}`, s.workers[w].ready ? [`<span class="tl-env result" data-job="${s.workers[w].ready}" title="result of job ${s.workers[w].ready}"></span>`] : [], false)).join("");
    return `
      <div class="tl-area tl-jobs"><div class="rx-kicker">jobs \u00B7 FIFO &lt;&lt;1, 2, 3&gt;&gt;</div><div class="tl-cards">${[1, 2, 3].map((j) => jobCard(MODE, j, "main", s)).join("")}</div></div>
      <div class="tl-area tl-done"><div class="rx-kicker">results collected</div><div class="tl-chips">${done || '<span class="tl-empty">none yet</span>'}</div></div>
      <div class="tl-area tl-values"><div class="rx-kicker">worker values</div><div class="tl-chips">${values || '<span class="tl-empty">none yet</span>'}</div></div>
      <div class="tl-area tl-gc"><div class="rx-kicker">global GC</div>${s.globalGC ? '<span class="tl-gc-done">ran once</span>' : '<span class="tl-empty">not yet</span>'}</div>
      <div class="tl-ports bottom">${results}${portHtml("M", "monitor", tokens, false)}</div>`;
  }

  const PHASE_TEXT = { idle: "idle", queued: "job waiting in mailbox", received: "has the job", ready: "result sent; waits for main to collect it", closing: "stopped: port closed, block returned", observed: "observed: value taken" };

  function workerRoom(MODE, s, w) {
    const W = s.workers[w];
    const processed = W.processed.map((j) => `<span class="tl-chip job">job ${j} <small>${MODE[j]}</small></span>`).join("");
    return `
      <div class="tl-ports top">${portHtml("D", "default port", W.mailbox ? [`<span class="tl-env" data-job="${W.mailbox}" title="job ${W.mailbox}"></span>`] : [], W.closed)}</div>
      <div class="tl-area tl-hand"><div class="rx-kicker">in hand</div><div class="tl-cards">${W.hand ? jobCard(MODE, W.hand, "worker", s) : '<span class="tl-empty">nothing</span>'}</div></div>
      <div class="tl-area tl-processed"><div class="rx-kicker">processed \u00B7 local GC \u00D7 ${W.gcs}</div><div class="tl-chips">${processed || '<span class="tl-empty">none yet</span>'}</div></div>
      <div class="tl-status">${PHASE_TEXT[W.phase]}</div>`;
  }

  function poolCaption(MODE, i, schedule, states) {
    if (i === 0) {
      return { step: "start", title: "Three jobs wait in main's FIFO. Both workers are idle.",
        text: ["Main may dispatch whenever a job is pending and a worker is idle. A worker sends its result only while no other result port holds one, so every Ractor.select below has exactly one ready port.", "Job 1 is shared (one frozen object on both sides), job 2 is copied (the worker gets a deep copy), job 3 is moved (main loses it)."],
        ruby: "pending = [1, 2, 3]\nworkers = { 1 => Ractor.new { ... }, 2 => Ractor.new { ... } }\nresults = { 1 => Ractor::Port.new, 2 => Ractor::Port.new }" };
    }
    const token = schedule[i - 1];
    const { action, worker: w } = decode(token);
    const before = states[i - 1];
    const W = before.workers[w];
    const step = `step ${i} / ${schedule.length} \u00B7 token ${token}`;
    switch (action) {
      case 1: {
        const j = before.pending[0];
        const mode = MODE[j];
        const detail = {
          share: "Job 1 is a frozen, shareable object. Sending it with move: true does not move anything: both sides now refer to the same object.",
          copy: "Job 2 is a mutable graph. Ruby deep-copies it; the worker gets the copy and main keeps the original.",
          move: "Job 3 is a mutable graph sent with move: true. Main's reference becomes a Ractor::MovedObject; only the worker can use the object now.",
        }[mode];
        const ruby = { share: `worker${w}.send(job1, move: true)\n# job1 is shareable: no copy, no move`, copy: `worker${w}.send(job2)\n# deep copy`, move: `worker${w}.send(job3, move: true)\njob3.size  # raises Ractor::MovedError` }[mode];
        return { step, title: `Main dispatches job ${j} to worker ${w}`, text: [`Main takes job ${j} from the front of its FIFO and sends it to worker ${w}'s default port D.`, detail], ruby };
      }
      case 2: return { step, title: `Worker ${w} receives job ${W.mailbox}`, text: `Worker ${w} takes job ${W.mailbox} out of its mailbox. In the model a worker receives only when main has nothing left to dispatch, so dispatches always come first.`, ruby: "job = Ractor.receive  # inside the worker" };
      case 3: return { step, title: `Worker ${w} processes job ${W.hand}`, text: [`Worker ${w} builds a retained heap and a temporary heap of ${HEAP} objects each for job ${W.hand}, runs one local GC, then sends the result to main's result port R${w}, where it queues.`, "A worker sends its result only while no other result port holds one, so main always knows which port will be ready."], ruby: `GC.start(global: false)  # local to this Ractor\nr${w}.send([:result, job, checksum, deltas])` };
      case 4: return { step, title: `Main collects worker ${w}'s result`, text: [`Main waits on both result ports at once. Only R${w} holds a message, so Ractor.select returns R${w} with the result of job ${W.ready}. Main marks job ${W.ready} done; worker ${w} is idle again.`], ruby: `port, message = Ractor.select(r1, r2)\nport.equal?(r${w})  # => true` };
      case 5: return { step, title: "Main runs one global GC", text: "All three jobs are done. Main runs one garbage collection that covers every Ractor's heap. Each worker's GC count rises by one more; their local major-GC count does not.", ruby: "GC.start(full_mark: true, immediate_mark: true,\n         immediate_sweep: true, global: true)" };
      case 6: return { step, title: `Main stops worker ${w}`, text: [`Main tells worker ${w} to stop. The worker closes its default port and its block returns. A send to it now raises Ractor::ClosedError.`, `The worker's termination token [worker ${w}, :exited] lands on main's monitor port M.`], ruby: `control${w}.send(:stop)\n# inside the worker:\nRactor.current.close\n[${w}, processed, retained_count]  # block returns` };
      case 7: {
        const jobs = W.processed;
        return { step, title: `Main observes worker ${w}`, text: [`Main receives [worker ${w}, :exited] from M, joins the worker, and takes its value: its id, the jobs it processed [${jobs.join(", ")}], and the ${HEAP * jobs.length} objects it retained.`, "A second value call raises Ractor::Error: the value was already taken."], ruby: `m.receive        # => [worker${w}, :exited]\nworker${w}.join     # => worker${w}\nworker${w}.value    # => [${w}, [${jobs.join(", ")}], ${HEAP * jobs.length}]` };
      }
    }
    return { step, title: ACTIONS[action] };
  }

  function mountPool(root, options) {
    const POOL = (window.RACTOR_MODELS || {}).RactorPool;
    if (!POOL) throw new Error("timeline pool view needs data/RactorPool.js");
    const MODE = POOL.jobs;
    const q = (sel, from = root) => from.querySelector(sel);

    root.classList.add("tl-pool");
    const panel = el("div", "rx-panel tl-player");
    const stage = el("div", "tl-stage");
    stage.innerHTML = `
      <div class="tl-lane lane-main rx-ractor main"><div class="tl-lane-name">main</div><div class="tl-room"></div></div>
      <div class="tl-lane lane-w1 rx-ractor worker"><div class="tl-lane-name">worker 1</div><div class="tl-room"></div></div>
      <div class="tl-lane lane-w2 rx-ractor worker2"><div class="tl-lane-name">worker 2</div><div class="tl-room"></div></div>
      <div class="tl-fx"></div>`;
    const fx = q(".tl-fx", stage);
    const track = el("div", "tl-track");
    const caption = el("div", "tl-caption");

    let schedule = POOL.schedules[0];
    let states = poolStates(MODE, schedule);
    let scheduleIndex = 0;
    let gen = 0;

    function render(s) {
      q(".lane-main .tl-room", stage).innerHTML = mainRoom(MODE, s);
      for (const w of [1, 2]) {
        const lane = q(`.lane-w${w}`, stage);
        lane.classList.toggle("closed", s.workers[w].closed);
        lane.classList.toggle("observed", s.workers[w].phase === "observed");
        q(".tl-room", lane).innerHTML = workerRoom(MODE, s, w);
      }
    }

    function paintTrack(i) {
      [...track.children].forEach((c, k) => {
        c.classList.toggle("done", k < i);
        c.classList.toggle("now", k === i - 1);
      });
    }

    async function onStep(i, animated, d) {
      const my = ++gen;
      const stale = () => my !== gen;
      const after = states[i];
      setCaption(caption, poolCaption(MODE, i, schedule, states));
      paintTrack(i);
      if (i === 0 || !animated) { render(after); return; }
      const before = states[i - 1];
      const { action, worker: w } = decode(schedule[i - 1]);
      render(before);
      const s = (sel) => q(sel, stage);
      switch (action) {
        case 1: {
          const j = before.pending[0];
          const src = snapshot(s(`.lane-main [data-job="${j}"]`));
          render(after);
          await flyBetween(fx, src, s(`.lane-w${w} [data-port="D"] .tl-env`), d * 0.85, { fade: true });
          if (stale()) return;
          pop(s(`.lane-main [data-job="${j}"]`));
          break;
        }
        case 2: {
          const src = snapshot(s(`.lane-w${w} [data-port="D"] .tl-env`));
          render(after);
          await flyBetween(fx, src, s(`.lane-w${w} .tl-hand .tl-job`), d * 0.7);
          break;
        }
        case 3: {
          const card = s(`.lane-w${w} .tl-hand .tl-job`);
          if (card) card.classList.add("working");
          await sleep(d * 0.5);
          if (stale()) return;
          await sweep(s(`.lane-w${w} .tl-room`), d * 0.55, "local GC", "var(--o-runs)");
          if (stale()) return;
          const src = snapshot(s(`.lane-w${w} .tl-hand .tl-job`), el("span", "tl-env result"));
          render(after);
          const port = s(`.lane-main [data-port="R${w}"]`);
          await flyBetween(fx, src, q(".tl-env", port), d * 0.7);
          if (stale()) return;
          port.classList.add("flash");
          await sleep(d * 0.3);
          port.classList.remove("flash");
          break;
        }
        case 4: {
          const src = snapshot(s(`.lane-main [data-port="R${w}"] .tl-env`));
          render(after);
          await flyBetween(fx, src, s(".lane-main .tl-done .tl-chip:last-child"), d * 0.7);
          if (stale()) return;
          pop(s(".lane-main .tl-done .tl-chip:last-child"));
          break;
        }
        case 5: {
          await sweep(stage, d * 1.1, "global GC", "var(--o-exit)");
          if (stale()) return;
          render(after);
          pop(s(".lane-main .tl-gc-done"));
          break;
        }
        case 6: {
          render(after);
          await sleep(d * 0.5);
          if (stale()) return;
          pop(s(`.lane-main [data-port="M"] [data-w="${w}"]`));
          break;
        }
        case 7: {
          const src = snapshot(s(`.lane-main [data-port="M"] [data-w="${w}"]`));
          render(after);
          await flyBetween(fx, src, s(`.lane-main .tl-values [data-w="${w}"]`), d * 0.7);
          break;
        }
      }
    }

    const player = new Player(schedule.length, onStep);

    function buildTrack() {
      track.innerHTML = "";
      schedule.forEach((token, k) => {
        const { action, worker } = decode(token);
        const c = el("div", `tl-cell a${action} w${worker}`, String(token));
        c.title = `${k + 1}. ${ACTIONS[action]}${worker ? ` (worker ${worker})` : ""}`;
        c.addEventListener("click", () => { player.pause(); player.go(k + 1, false); });
        track.append(c);
      });
    }

    const pick = el("span", "tl-group");
    pick.append(el("span", "tl-label", "Schedule"));
    const prev = el("button", "rx-btn", "\u2039");
    prev.type = "button";
    prev.title = "Previous schedule";
    const next = el("button", "rx-btn", "\u203A");
    next.type = "button";
    next.title = "Next schedule";
    const select = el("select", "rx-select tl-schedule");
    select.setAttribute("aria-label", "Schedule");
    POOL.schedules.forEach((s, k) => select.append(new Option(`#${k + 1}  ${s.join(" ")}`, String(k))));
    pick.append(prev, select, next);

    let rows = [];
    function selectSchedule(k, play) {
      scheduleIndex = (k + POOL.schedules.length) % POOL.schedules.length;
      schedule = POOL.schedules[scheduleIndex];
      states = poolStates(MODE, schedule);
      select.value = String(scheduleIndex);
      buildTrack();
      rows.forEach((r, i) => r.classList.toggle("selected", i === scheduleIndex));
      const done = player.reset(schedule.length);
      if (play) done.then(() => player.play());
    }
    select.addEventListener("change", () => selectSchedule(Number(select.value), false));
    prev.addEventListener("click", () => selectSchedule(scheduleIndex - 1, false));
    next.addEventListener("click", () => selectSchedule(scheduleIndex + 1, false));

    panel.append(buildControls(player, pick), stage, track, caption);
    root.append(panel);
    buildTrack();
    render(states[0]);
    setCaption(caption, poolCaption(MODE, 0, schedule, states));

    rows = buildBraid(root, POOL, scheduleIndex, options.braid === "open", (k) => {
      selectSchedule(k, true);
      panel.scrollIntoView({ behavior: reduced ? "auto" : "smooth", block: "start" });
    });
  }

  // -------------------------------------------------------------- braid

  function buildBraid(root, POOL, selected, open, onPick) {
    const n = POOL.schedules.length;
    const tail = POOL.schedules[0].slice(-5);
    const sameTail = POOL.schedules.every((s) => s.slice(-5).join() === tail.join());

    const box = el("div", "tl-braid-box");
    const toggle = el("button", "tl-braid-toggle", "");
    toggle.type = "button";
    toggle.setAttribute("aria-expanded", String(open));
    const body = el("div", "tl-braid-body");
    body.hidden = !open;
    const setLabel = () => { toggle.textContent = `${body.hidden ? "\u25B8" : "\u25BE"} All ${n} schedules`; };
    toggle.addEventListener("click", () => { body.hidden = !body.hidden; toggle.setAttribute("aria-expanded", String(!body.hidden)); setLabel(); });
    setLabel();

    const lede = el("p", "tl-braid-lede");
    lede.textContent = `TLC found ${n} complete schedules in the bounded model (${POOL.states} states). Each row is one schedule; each cell is one step, coloured by action, in a lighter shade for worker 2. ${sameTail ? `Every schedule ends with the same five steps, ${tail.join(", ")}: the global GC, then stop and observe worker 1, then stop and observe worker 2.` : ""} CRuby replayed all ${n} schedules and every replay passed. Hover a row to read it; click a row to play it.`;

    const legend = el("div", "tl-legend");
    for (const a of [1, 2, 3, 4, 5, 6, 7]) {
      const s = el("span");
      s.append(el("i", `a${a}${a === 5 ? "" : " half"}`), document.createTextNode(ACTIONS[a] + (a === 5 ? "" : " (W1 | W2)")));
      legend.append(s);
    }

    const grid = el("div", "tl-braid");
    const tip = el("div", "tl-tip");
    tip.hidden = true;
    const cols = [el("div", "tl-braid-col"), el("div", "tl-braid-col")];
    for (const col of cols) {
      const head = el("div", "tl-braid-head");
      head.append(el("span"));
      for (let k = 1; k <= 12; k++) head.append(el("span", null, String(k)));
      head.append(el("span", "tail", "same five steps"));
      col.append(head);
    }
    const half = Math.ceil(n / 2);
    const rows = POOL.schedules.map((s, k) => {
      const row = el("div", "tl-row");
      row.classList.toggle("selected", k === selected);
      row.append(el("span", "tl-row-n", String(k + 1)));
      for (const token of s) {
        const { action, worker } = decode(token);
        row.append(el("i", `tl-dot a${action} w${worker}`));
      }
      row.addEventListener("mouseenter", () => {
        tip.innerHTML = `<b>Schedule #${k + 1}</b><div>${s.map((t) => { const { action, worker } = decode(t); return `${ACTIONS[action]}${worker ? ` W${worker}` : ""}`; }).join(" \u203A ")}</div><div class="tl-tip-tokens">${s.join(" ")}</div>`;
        tip.hidden = false;
      });
      row.addEventListener("mousemove", (e) => {
        const x = Math.min(e.clientX + 14, window.innerWidth - tip.offsetWidth - 10);
        const y = e.clientY + 16 + tip.offsetHeight > window.innerHeight ? e.clientY - tip.offsetHeight - 10 : e.clientY + 16;
        tip.style.left = `${x}px`;
        tip.style.top = `${y}px`;
      });
      row.addEventListener("mouseleave", () => { tip.hidden = true; });
      row.addEventListener("click", () => onPick(k));
      cols[k < half ? 0 : 1].append(row);
      return row;
    });
    grid.append(...cols);
    body.append(lede, legend, grid, tip);
    box.append(toggle, body);
    root.append(box);
    return rows;
  }

  // --------------------------------------------------------------- fork

  function forkPath(model) {
    const base = RX.shortest(model, (t, s) => t.op === "Fork" && s.fields.ts[0] === "blocked" && s.fields.ts[1] === "blocked" && s.fields.atExit, "fork");
    if (!base) return null;
    const path = [...base];
    let cur = path[path.length - 1].to;
    for (const op of ["Count", "SendWorker", "Join", "Value", "MainReturn"]) {
      const t = model.out[cur].find((x) => x.op === op);
      if (!t) break;
      path.push(t);
      cur = t.to;
    }
    return path;
  }

  function mountFork(root) {
    if (!window.RX) throw new Error("timeline fork view needs common.js");
    const model = RX.load("RactorProcess");
    root.classList.add("tl-fork");
    const panel = el("div", "rx-panel tl-player");
    const wrap = el("div", "tl-fork-wrap");
    const svgRoot = svg("svg", { class: "tl-fork-svg", role: "img", "aria-label": "Timeline of a fork in a process with Ractors" });
    wrap.append(svgRoot);
    const captionEl = el("div", "tl-caption");
    const path = forkPath(model);
    if (!path) {
      setCaption(captionEl, { title: "The model has no fork scenario with both threads blocked.", text: "" });
      panel.append(captionEl);
      root.append(panel);
      return;
    }
    const stateAt = (c) => (c === 0 ? path[0].from : path[c - 1].to);
    const forkCol = path.findIndex((t) => t.op === "Fork") + 1;
    const cols = path.length + 1;
    const X0 = 200, DX = 100, PAD = 30;
    const W = X0 + (cols - 1) * DX + 150;
    const yTop = [72, 132, 192];
    const yChild = [330, 390, 450];
    const H = 500;
    const xOf = (c) => X0 + c * DX;
    const lanes = [
      { id: 0, label: "main thread", sub: "Ractor main", color: "c-main" },
      { id: 1, label: "T1", sub: "Ractor main \u00B7 p1.receive", color: "c-main" },
      { id: 2, label: "T2", sub: "worker \u00B7 Ractor.receive", color: "c-worker" },
    ];
    svgRoot.setAttribute("viewBox", `0 0 ${W} ${H}`);

    const bars = svg("g");
    for (let c = 0; c < cols; c++) bars.append(svg("rect", { class: "colbar", "data-col": c, x: xOf(c) - 40, y: 22, width: 80, height: H - 44, rx: 10 }));
    svgRoot.append(bars);

    const sideLabel = (x, y, text) => svg("text", { class: "side-label", x, y }, text);
    svgRoot.append(sideLabel(PAD, 36, "before fork"));

    lanes.forEach((l, k) => {
      const g = svg("g", { class: l.color });
      g.append(svg("text", { class: "lane-label", x: PAD, y: yTop[k] + 4 }, l.label));
      g.append(svg("text", { class: "lane-sub", x: PAD, y: yTop[k] + 19 }, l.sub));
      g.append(svg("line", { class: "lane-line", x1: X0 - 30, y1: yTop[k], x2: xOf(forkCol), y2: yTop[k], stroke: "var(--c)" }));
      svgRoot.append(g);
    });

    function threadIcon(x, y, status, color) {
      const g = svg("g", { class: color });
      g.append(svg("circle", { class: `th ${status}`, cx: x, cy: y, r: 9 }));
      if (status === "dead") g.append(svg("text", { class: "th-x", x, y: y + 4 }, "\u2715"));
      if (status === "blocked") {
        g.append(svg("text", { class: "zz", x: x + 11, y: y - 9 }, "z"));
        g.append(svg("text", { class: "zz z2", x: x + 18, y: y - 15 }, "z"));
      }
      return g;
    }

    const afterFork = svg("g", { class: "col", "data-col": forkCol });
    const xf = xOf(forkCol);
    afterFork.append(sideLabel(xf + 48, 36, "parent"));
    const parentNote = svg("text", { class: "note", x: xf + 48, y: yTop[2] + 38 });
    parentNote.append(svg("tspan", { x: xf + 48, dy: 0 }, "The parent keeps its threads and waits for the child to exit."));
    parentNote.append(svg("tspan", { x: xf + 48, dy: 15 }, "The model follows the child from here."));
    afterFork.append(parentNote);
    lanes.forEach((l, k) => {
      afterFork.append(svg("line", { class: `lane-line parent ${l.color}`, x1: xf, y1: yTop[k], x2: W - 20, y2: yTop[k], stroke: "var(--c)" }));
    });
    afterFork.append(sideLabel(PAD, yChild[0] - 36, "child process"));
    lanes.forEach((l, k) => {
      const g = svg("g", { class: l.color });
      g.append(svg("text", { class: "lane-label", x: PAD, y: yChild[k] + 4 }, l.label));
      g.append(svg("text", { class: "lane-sub", x: PAD, y: yChild[k] + 19 }, k === 0 ? "the only thread in the child" : "gone in the child"));
      g.append(svg("path", { class: "split", d: `M ${xf - DX * 0.55} ${yTop[k]} C ${xf - 10} ${yTop[k]}, ${xf - 70} ${yChild[k]}, ${xf} ${yChild[k]}`, stroke: "var(--c)" }));
      if (k === 0) {
        g.append(svg("line", { class: "lane-line", x1: xf, y1: yChild[k], x2: W - 20, y2: yChild[k], stroke: "var(--c)" }));
      } else {
        g.append(svg("line", { class: "lane-line", x1: xf, y1: yChild[k], x2: xf + 26, y2: yChild[k], stroke: "var(--r-dead)" }));
        g.append(threadIcon(xf + 40, yChild[k], "dead", "c-dead"));
        g.append(svg("text", { class: "gone", x: xf + 58, y: yChild[k] + 4 }, k === 1 ? "gone: never wakes, no ensure runs" : "gone with the worker: no ensure runs"));
      }
      afterFork.append(g);
    });
    svgRoot.append(afterFork);

    const shortRuby = (t) => {
      const d = RX.describe(model, t);
      const out = { Block: d.ruby, AtExit: "at_exit { ... }", Fork: "fork", Count: "Ractor.count", SendWorker: "worker.send(:msg)", Join: "worker.join", Value: "worker.value", MainReturn: "# script ends" }[t.op] || d.ruby;
      const value = { self: "worker", child: "nil (child)" }[t.value] || t.value;
      const res = { blocks: "waits", nil: "\u2192 nil", exit: `exit ${t.exit ? t.exit.status : ""}`.trim() }[d.kind] || (d.kind === "err" ? `raises ${t.outcome.replace(/:[a-z]+$/, "")}` : value ? `\u2192 ${value}` : "ok");
      return { ruby: out, res, kind: d.kind };
    };

    for (let c = 0; c < cols; c++) {
      const scene = RX.scene(model, stateAt(c));
      const t = c > 0 ? path[c - 1] : null;
      const actor = t ? (t.op === "Block" ? t.args[0] : 0) : null;
      const isChild = c >= forkCol;
      const g = svg("g", { class: "col", "data-col": c });
      const x = xOf(c);
      if (!isChild || c === forkCol) {
        for (const th of scene.threads) {
          const k = th.id;
          const status = c === forkCol ? RX.scene(model, stateAt(c - 1)).threads[k].status : th.status;
          g.append(threadIcon(x, yTop[k], status, lanes[k].color));
        }
      }
      if (isChild) g.append(threadIcon(x, yChild[0], scene.threads[0].status, lanes[0].color));
      if (t) {
        const { ruby, res, kind } = shortRuby(t);
        const y = isChild && c > forkCol ? yChild[0] : yTop[actor];
        const label = svg("g", { class: `k-${kind}` });
        const tw = Math.max(ruby.length * 7.2 + 16, 40);
        const lift = c > forkCol && c % 2 === 0 ? 24 : 0;
        label.append(svg("rect", { class: "badge-bg", x: x - tw / 2, y: y - 42 - lift, width: tw, height: 20 }));
        label.append(svg("text", { class: "ruby", x, y: y - 28 - lift }, ruby));
        if (lift) label.append(svg("line", { x1: x, y1: y - 22 - lift, x2: x, y2: y - 14, stroke: "currentColor", "stroke-width": 1, opacity: .5 }));
        const drop = c >= forkCol && c % 2 === 0 ? 44 : 28;
        if (t.op === "Fork") {
          label.append(svg("text", { class: "outcome k-ok", x, y: yChild[0] + drop }, "\u2192 nil: this is the child"));
        } else {
          label.append(svg("text", { class: `outcome k-${kind}${kind === "err" ? " err-flash" : ""}`, x, y: y + drop }, res));
        }
        if (t.op === "MainReturn" && t.exit && t.exit.atExit) {
          label.append(svg("text", { class: "outcome k-exit", x, y: yChild[0] + drop + 16 }, "at_exit ran"));
        }
        g.append(label);
      }
      svgRoot.append(g);
    }

    function show(i, animated) {
      svgRoot.querySelectorAll(".col").forEach((g) => g.classList.toggle("revealed", Number(g.dataset.col) <= i));
      svgRoot.querySelectorAll(".colbar").forEach((b) => b.classList.toggle("now", Number(b.dataset.col) === i));
      svgRoot.querySelectorAll(".err-flash").forEach((n) => {
        if (Number(n.closest(".col").dataset.col) === i && animated) {
          n.classList.remove("err-flash");
          void n.getBoundingClientRect();
          n.classList.add("err-flash");
        }
      });
      if (i === 0) {
        setCaption(captionEl, { step: "start", title: "Three threads, all idle", text: ["Main's main thread will drive the scenario. T1 belongs to main; T2 belongs to the worker. Port P1 belongs to main and is empty."], ruby: "p1 = Ractor::Port.new\nworker = Ractor.new { Ractor.receive; :finished }\nt1 = Thread.new { p1.receive }" });
        return;
      }
      const t = path[i - 1];
      const d = RX.describe(model, t);
      const extra = [];
      if (t.op === "Fork") extra.push("The child has only the forking thread. T1 and the worker's threads are gone without unwinding: no ensure clause runs. The child sees the worker as terminated, and its value is nil once.");
      if (t.op === "SendWorker") extra.push("The worker's default port is closed in the child, so the send fails.");
      if (t.op === "Value") extra.push("The worker never finished, yet value does not block and does not raise: in the child of a fork it returns nil, once.");
      setCaption(captionEl, { step: `step ${i} / ${path.length}`, title: d.title, kind: d.kind, text: [...d.sentences, ...extra], ruby: d.ruby, note: t.op === "MainReturn" ? "CRuby replayed this path; the replay passed." : null });
    }

    const player = new Player(path.length, async (i, animated, d) => {
      show(i, animated);
      if (animated) await sleep(d * 0.6);
    });
    panel.append(buildControls(player), wrap, captionEl);
    root.append(panel);
    show(0, false);
  }

  window.RXWidgets = window.RXWidgets || {};
  window.RXWidgets.timeline = {
    mount(root, options = {}) {
      root.classList.add("tl");
      if (options.view === "fork") mountFork(root, options);
      else mountPool(root, options);
    },
  };
})();
