(function () {
  "use strict";

  const PALETTE = ["#6366f1", "#d97706", "#db2777", "#0891b2", "#2563eb", "#a16207", "#9333ea", "#64748b", "#16a34a", "#e11d48", "#0d9488", "#7c3aed", "#ca8a04", "#475569"];
  const reduced = matchMedia("(prefers-reduced-motion: reduce)");
  const darkMedia = matchMedia("(prefers-color-scheme: dark)");

  function esc(s) { return String(s).replace(/[&<>"]/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" })[c]); }
  function rgba(hex, a) {
    const h = hex.replace("#", "");
    const n = parseInt(h.length === 3 ? h.split("").map((c) => c + c).join("") : h, 16);
    return `rgba(${(n >> 16) & 255},${(n >> 8) & 255},${n & 255},${a})`;
  }

  // ------------------------------------------------------------ graph data

  function build(name) {
    const model = RX.load(name);
    const n = model.cores.length;
    const depth = new Array(n).fill(-1);
    const queue = [...model.initial];
    model.initial.forEach((id) => (depth[id] = 0));
    while (queue.length) {
      const id = queue.shift();
      for (const t of model.out[id]) if (depth[t.to] < 0) { depth[t.to] = depth[id] + 1; queue.push(t.to); }
    }
    const maxDepth = Math.max(...depth);
    const edgeMap = new Map();
    const edges = [];
    for (const t of model.transitions) {
      if (t.from === t.to) continue;
      const key = `${t.from}>${t.to}`;
      let e = edgeMap.get(key);
      if (!e) { e = { id: edges.length, a: t.from, b: t.to, tids: [], choice: false }; edgeMap.set(key, e); edges.push(e); }
      e.tids.push(t.id);
      if (t.choice) e.choice = true;
    }
    const edgeOfT = new Array(model.transitions.length).fill(-1);
    for (const e of edges) for (const id of e.tids) edgeOfT[id] = e.id;
    const selfLoops = model.transitions.length - edgeOfT.filter((x) => x >= 0).length;
    return { model, n, depth, maxDepth, edges, edgeOfT, selfLoops, paths: replayPaths(model) };
  }

  // Same forest as api/verify_api.rb paths_for: breadth-first from the
  // initial states, delaying choice transitions to the next layer.
  function replayPaths(model) {
    const parent = new Map(model.initial.map((id) => [id, null]));
    const order = [];
    let queue = [...model.initial];
    while (queue.length) {
      const crossings = [];
      while (queue.length) {
        const id = queue.shift();
        order.push(id);
        for (const t of model.out[id]) {
          if (parent.has(t.to)) continue;
          if (t.choice) { crossings.push(t); continue; }
          parent.set(t.to, t);
          queue.push(t.to);
        }
      }
      for (const t of crossings) {
        if (parent.has(t.to)) continue;
        parent.set(t.to, t);
        queue.push(t.to);
      }
    }
    const routeTo = (id) => { const steps = []; let t; while ((t = parent.get(id))) { steps.unshift(t); id = t.from; } return steps; };
    const paths = [];
    for (const id of order) {
      const leaf = !model.out[id].some((t) => parent.get(t.to) === t);
      if (leaf && parent.get(id)) paths.push(routeTo(id));
    }
    for (const t of model.transitions) if (parent.get(t.to) !== t) paths.push([...routeTo(t.from), t]);
    return paths;
  }

  // --------------------------------------------------------------- layout

  function layout(g, W, H) {
    const { model, depth, maxDepth, n } = g;
    const aspects = model.aspects;
    const multi = aspects.length > 1;
    const count = {};
    for (const c of model.cores) count[c.fields.aspect] = (count[c.fields.aspect] || 0) + 1;
    const gap = multi ? Math.min(0.16, 1.1 / aspects.length) : 0;
    const total = 2 * Math.PI - gap * aspects.length;
    const weight = aspects.reduce((s, b) => s + Math.sqrt(count[b]), 0);
    const sectors = {};
    let start = -Math.PI / 2 + gap / 2;
    for (const a of aspects) {
      const span = total * Math.sqrt(count[a]) / weight;
      sectors[a] = { start, span, mid: start + span / 2 };
      start += span + gap;
    }
    const base = multi ? 0.55 : 0;
    const maxRad = maxDepth + base;
    const margin = 36;
    const unitX = Math.min((W / 2 - margin) / maxRad, 120);
    const unitY = Math.min((H / 2 - margin) / maxRad, unitX * 0.78);
    const cx = W / 2, cy = H / 2;
    const rings = Array.from({ length: maxDepth + 1 }, () => []);
    for (let i = 0; i < n; i++) rings[depth[i]].push(i);
    const angle = new Float64Array(n);
    const pos = Array.from({ length: n }, () => ({ x: cx, y: cy }));
    const norm = (a, s) => { let x = a - s; while (x < 0) x += 2 * Math.PI; while (x >= 2 * Math.PI) x -= 2 * Math.PI; return x; };
    let minPitch = Infinity;
    rings.forEach((ring, d) => {
      const rad = d + base;
      for (const a of aspects) {
        const ids = ring.filter((id) => model.cores[id].fields.aspect === a);
        if (!ids.length) continue;
        const sec = sectors[a];
        const bary = new Map();
        for (const id of ids) {
          let sx = 0, sy = 0;
          for (const t of model.inn[id]) if (depth[t.from] === d - 1) { sx += Math.cos(angle[t.from]); sy += Math.sin(angle[t.from]); }
          bary.set(id, sx || sy ? norm(Math.atan2(sy, sx), sec.start) : sec.span / 2);
        }
        ids.sort((p, q) => bary.get(p) - bary.get(q) || p - q);
        if (d === 0 && ids.length === 1) {
          angle[ids[0]] = sec.mid;
          pos[ids[0]] = multi ? { x: cx + Math.cos(sec.mid) * unitX * rad, y: cy + Math.sin(sec.mid) * unitY * rad } : { x: cx, y: cy };
          continue;
        }
        ids.forEach((id, i) => {
          const a0 = sec.start + sec.span * (i + 0.5) / ids.length;
          angle[id] = a0;
          pos[id] = { x: cx + Math.cos(a0) * unitX * rad, y: cy + Math.sin(a0) * unitY * rad };
        });
        if (ids.length > 1) {
          const r = Math.sqrt((unitX * rad) ** 2 + (unitY * rad) ** 2) / Math.SQRT2;
          minPitch = Math.min(minPitch, sec.span * r / ids.length);
        }
      }
    });
    const nodeR = Math.max(2.4, Math.min(6, (isFinite(minPitch) ? minPitch : 20) * 0.34));
    return { pos, angle, rings, sectors, unitX, unitY, cx, cy, base, nodeR, multi };
  }

  // ------------------------------------------------------------- schedule

  function schedule(g, L) {
    const { model, depth, n } = g;
    const appear = new Float64Array(n).fill(Infinity);
    const expand = new Float64Array(n).fill(Infinity);
    let t = 0;
    model.initial.forEach((id) => (appear[id] = 0));
    const ringStart = [];
    L.rings.forEach((ring, d) => {
      const dur = Math.min(2200, 650 + ring.length * 10);
      ringStart.push({ t, dur });
      const sorted = [...ring].sort((a, b) => L.angle[a] - L.angle[b]);
      sorted.forEach((id, i) => {
        const at = t + 250 + (dur - 250) * i / Math.max(1, sorted.length);
        expand[id] = at;
        for (const tr of model.out[id]) if (depth[tr.to] === d + 1) appear[tr.to] = Math.min(appear[tr.to], at);
      });
      t += dur;
    });
    const edgeAt = g.edges.map((e) => expand[e.a]);
    const transByExpand = Array.from({ length: n }, (_, i) => i).sort((a, b) => expand[a] - expand[b]);
    const appearSorted = Array.from(appear).sort((a, b) => a - b);
    return { appear, expand, edgeAt, total: t + 500, ringStart, transByExpand, appearSorted };
  }

  // --------------------------------------------------------------- widget

  const TEMPLATE = `
<div class="rx-panel mv-toolbar">
  <label class="mv-picker">
    <span class="rx-kicker">Model</span>
    <select class="rx-select mv-model"></select>
  </label>
  <ol class="mv-steps">
    <li data-act="1"><button type="button"><span class="n">1</span><span class="t">Every dot is a state</span></button></li>
    <li data-act="2"><button type="button"><span class="n">2</span><span class="t">TLC visits every state</span></button></li>
    <li data-act="3"><button type="button"><span class="n">3</span><span class="t">Every line runs on CRuby</span></button></li>
  </ol>
  <div class="mv-controls">
    <button type="button" class="rx-btn primary mv-play">Start the search</button>
    <button type="button" class="rx-btn mv-skip" hidden>Skip to the end</button>
    <button type="button" class="rx-btn mv-restart">Restart</button>
  </div>
</div>
<div class="mv-stage">
  <div class="rx-panel mv-canvas-wrap">
    <canvas class="mv-canvas" aria-label="State graph of the model"></canvas>
    <div class="mv-hud">
      <div class="mv-counter mv-c-states"><b>0</b><span>states found</span><small></small></div>
      <div class="mv-counter mv-c-trans"><b>0</b><span>transitions found</span><small></small></div>
      <div class="mv-counter mv-c-paths" hidden><b>0</b><span>replays on CRuby</span><small></small></div>
    </div>
    <div class="mv-banner" hidden></div>
    <div class="mv-tip" hidden></div>
  </div>
  <aside class="mv-side">
    <div class="rx-panel mv-caption"></div>
    <div class="rx-panel mv-detail"></div>
    <div class="rx-panel mv-legend">
      <div class="mv-legend-head">
        <span class="rx-kicker">Colour by</span>
        <select class="rx-select mv-colour"></select>
      </div>
      <ul class="mv-swatches"></ul>
      <ul class="mv-lines">
        <li><svg width="34" height="10"><line x1="1" y1="5" x2="33" y2="5" class="l-plain"/></svg>transition: one call, one outcome</li>
        <li><svg width="34" height="10"><line x1="1" y1="5" x2="33" y2="5" class="l-choice"/></svg>choice: the same call may end another way</li>
        <li><svg width="34" height="10"><line x1="1" y1="5" x2="33" y2="5" class="l-ok"/></svg>replayed on CRuby</li>
        <li><svg width="34" height="12"><circle cx="17" cy="6" r="5" class="n-init"/></svg>starting state</li>
      </ul>
    </div>
  </aside>
</div>`;

  const EMPTY_DETAIL = `<p class="rx-muted mv-empty">Hover a dot to see its lines. Click a dot to look inside the state.</p>`;

  // The start state of a model in one sentence, from RX.scene.
  function stateInWords(m, id) {
    const s = RX.scene(m, id);
    const parts = [];
    const ractors = s.ractors.map((r) => r.name);
    parts.push(`${ractors.slice(0, -1).join(", ")}${ractors.length > 1 ? " and " : ""}${ractors.at(-1)} run`);
    if (s.ports.length) {
      const open = s.ports.filter((p) => p.open && !p.messages.length).map((p) => p.label);
      if (open.length === s.ports.length) parts.push(`port${open.length > 1 ? "s" : ""} ${open.join(", ")} ${open.length > 1 ? "are" : "is"} open and empty`);
    }
    if (s.threads.length && s.threads.every((t) => t.status === "idle")) parts.push("no thread waits");
    s.facts.slice(0, 2).forEach((f) => parts.push(`${f.label}: ${f.value}`));
    return parts.join("; ");
  }

  function firstStep(m, id) {
    const t = m.out[id].find((x) => x.to !== x.from);
    if (!t) return "";
    const d = RX.describe(m, t);
    return ` For example, after <i>${esc(d.title)}</i> (<code>${esc(d.ruby)}</code>), the model is in a new state one ring out.`;
  }

  function pathInWords(m, path) {
    return path.map((t) => `<li>${esc(RX.describe(m, t).title)} <span class="rx-badge ${RX.kind(t.outcome)}">${esc(t.outcome)}</span></li>`).join("");
  }

  const ACT_TEXT = {
    1: (m) => {
      const starts = m.initial.length;
      const start = m.initial[0];
      const per = starts > 1
        ? `This model has <span class="num">${starts}</span> starting dots, one per scenario (${m.aspects.map((a) => `<code>${esc(a)}</code>`).join(", ")}). In the first one, ${esc(stateInWords(m, start))}.`
        : `The large dot is the start: ${esc(stateInWords(m, start))}.`;
      return `<h2>Every dot is a state</h2><p>A state is a snapshot of everything the model tracks: which Ractors run, which ports are open, what each port holds, what each thread is doing. ${per}${firstStep(m, start)}</p><p class="rx-muted">Click a dot to look inside it. Press <b>Start the search</b> to let TLC explore from here.</p>`;
    },
    2: (m, g) => `<h2>TLC visits every state</h2><p>From each state, TLC tries every call that any Ractor could make next, the valid calls and the invalid ones. Each call leads to a state. A state that TLC has not seen before joins the search. The search stops when no call leads anywhere new. Each ring is one more call from the start, so the outer rings hold the states that need the longest call sequences.</p><p>When the counters stop, the picture holds every state this model allows, within its bounds. A unit test checks one sequence of calls; this search covers all of them.</p><p class="rx-muted">TLC saw <span class="num">${m.counts.states}</span> states. Merging states that differ only in the record of the last call leaves the <span class="num">${m.counts.core_states}</span> dots drawn here. <span class="num">${g.selfLoops}</span> of the <span class="num">${m.counts.transitions}</span> transitions are calls that change nothing, most of them errors; they are counted but not drawn.</p>`,
    3: (m, g) => {
      const example = g.paths.reduce((best, p) => (p.length > best.length && p.length <= 4 ? p : best), g.paths[0] || []);
      return `<h2>Every line runs on CRuby</h2><p>The model is only useful if CRuby behaves the same way. The verifier cuts the graph into <span class="num">${m.counts.paths}</span> call sequences that together use every line at least once. Each sequence runs from the start in a fresh Ruby process, which makes the same calls and checks every outcome and every visible effect against the model. A line turns green when its sequence has passed.</p><p>One of the sequences:</p><ol class="mv-example">${pathInWords(m, example)}</ol><p>${g.edges.some((e) => e.choice) ? `Dashed lines are choices: the same call from the same state may end in more than one way, and CRuby takes one of them. When CRuby takes another allowed outcome, the replay stops there and passes; a line behind a choice may be one that CRuby never took.` : `This model has no choices: every call from every state has exactly one outcome, so CRuby must follow each sequence to its end.`}</p>`;
    },
  };

  function mount(root, options) {
    options = options || {};
    root.classList.add("mv");
    root.innerHTML = TEMPLATE;
    const q = (s) => root.querySelector(s);
    const canvas = q(".mv-canvas");
    const ctx = canvas.getContext("2d");
    const wrap = q(".mv-canvas-wrap");
    const tip = q(".mv-tip");
    const picker = q(".mv-model");
    const names = RX.supported();
    if (!names.length) { root.innerHTML = `<p class="rx-muted">No model data is loaded.</p>`; return; }
    for (const n of names) {
      const o = document.createElement("option");
      o.value = n;
      o.textContent = `${n} — ${RACTOR_MODELS[n].summary}`;
      picker.appendChild(o);
    }
    if (options.picker === "off") q(".mv-picker").hidden = true;
    picker.value = names.includes(options.model) ? options.model : names[0];

    let theme = {};
    function readTheme() {
      const cs = getComputedStyle(root);
      const v = (n) => cs.getPropertyValue(n).trim();
      theme = { fg: v("--fg"), muted: v("--muted"), border: v("--border"), accent: v("--accent"), ok: v("--o-ok"), blocks: v("--o-blocks"), edge: darkMedia.matches ? 0.55 : 0.3 };
    }

    let G, L, S;
    let act = 1;
    let playing = false;
    let clock = 0;
    let lastFrame = 0;
    let act2Done = false, act3Done = false;
    let hover = -1, selected = -1, hoverEdge = -1;
    let colourBy = "aspect";
    let colourKey = [], colourOf = [];
    let replay = null;
    let W = 0, H = 0, dpr = 1;
    let visible = true;
    let rate = { r0: 0.9 / 1000, a: Math.LN2 / 1100, rmax: 420 / 1000 };

    function resize() {
      const r = wrap.getBoundingClientRect();
      if (!r.width || !r.height) return;
      dpr = window.devicePixelRatio || 1;
      W = Math.round(r.width); H = Math.round(r.height);
      canvas.width = W * dpr; canvas.height = H * dpr;
      if (G) { L = layout(G, W, H); S = schedule(G, L); }
    }

    function setModel(name) {
      G = build(name);
      L = layout(G, W, H);
      S = schedule(G, L);
      rate = { r0: 0.9 / 1000, a: Math.LN2 / 1100, rmax: Math.max(420, G.paths.length / 8) / 1000 };
      act = 1; playing = false; act2Done = false; act3Done = false; clock = 0;
      hover = -1; selected = -1; hoverEdge = -1; replay = null;
      tip.hidden = true;
      buildColourOptions();
      applyColour("aspect");
      renderDetail();
      renderStep();
      updateCounters(0);
      const expected = G.model.counts.paths;
      if (G.paths.length !== expected) console.warn(`${name}: computed ${G.paths.length} replay paths, data says ${expected}`);
    }

    // --------------------------------------------------------------- colour

    function buildColourOptions() {
      const sel = q(".mv-colour");
      sel.innerHTML = "";
      const opts = [["aspect", "scenario (aspect)"]];
      for (const v of G.model.variables) {
        if (v.name === "aspect" || v.name === "step") continue;
        const vals = new Set(G.model.cores.map((c) => JSON.stringify(c.fields[v.name])));
        if (vals.size >= 2 && vals.size <= PALETTE.length) opts.push([v.name, v.name]);
      }
      for (const [val, label] of opts) { const o = document.createElement("option"); o.value = val; o.textContent = label; sel.appendChild(o); }
    }

    function applyColour(key) {
      colourBy = key;
      q(".mv-colour").value = key;
      const vals = G.model.cores.map((c) => (key === "aspect" ? c.fields.aspect : JSON.stringify(c.fields[key])));
      colourKey = [...new Set(vals)].sort();
      colourOf = vals.map((v) => PALETTE[colourKey.indexOf(v) % PALETTE.length]);
      const ul = q(".mv-swatches");
      ul.innerHTML = "";
      colourKey.forEach((v, i) => {
        const li = document.createElement("li");
        li.style.setProperty("--c", PALETTE[i % PALETTE.length]);
        li.innerHTML = `<i></i>${esc(v)}`;
        ul.appendChild(li);
      });
    }
    const aspectColour = (a) => PALETTE[G.model.aspects.indexOf(a) % PALETTE.length];

    // ---------------------------------------------------------------- acts

    function renderStep() {
      const m = G.model;
      root.querySelectorAll(".mv-steps li").forEach((li) => {
        const a = Number(li.dataset.act);
        li.classList.toggle("here", a === act);
        li.classList.toggle("done", (a === 2 && act2Done) || (a === 3 && act3Done) || (a === 1 && act > 1));
      });
      q(".mv-caption").innerHTML = ACT_TEXT[act](m, G);
      const play = q(".mv-play"), skip = q(".mv-skip");
      skip.hidden = !playing;
      play.hidden = playing;
      if (act === 1) play.textContent = "Start the search";
      else if (act === 2) play.textContent = act2Done ? "Next: run every line on CRuby" : "Play the search";
      else play.textContent = act3Done ? "Watch again" : "Replay every path";
      q(".mv-c-paths").hidden = act < 3;
      q(".mv-c-states small").textContent = `of ${m.counts.core_states}`;
      q(".mv-c-trans small").textContent = `of ${m.counts.transitions}`;
      q(".mv-c-paths small").textContent = `of ${m.counts.paths}`;
      const banner = q(".mv-banner");
      banner.hidden = !(act === 3 && act3Done);
      banner.innerHTML = `<span class="rx-badge ok">PASS</span> All ${m.counts.paths} replays passed on CRuby`;
    }

    function startAct2() {
      act = 2; act2Done = false; act3Done = false; replay = null;
      clock = 0; playing = true;
      if (reduced.matches) finishAct2();
      renderStep();
    }
    function finishAct2() { clock = S.total; playing = false; act2Done = true; renderStep(); }

    function startAct3() {
      if (!act2Done) { clock = S.total; act2Done = true; }
      act = 3; act3Done = false;
      replay = { t: 0, done: 0, flashes: [], verifiedE: new Uint8Array(G.edges.length), verifiedN: new Uint8Array(G.n) };
      playing = true;
      if (reduced.matches) finishAct3();
      renderStep();
    }
    function finishAct3() {
      replay.done = G.paths.length;
      replay.verifiedE.fill(1); replay.verifiedN.fill(1);
      replay.flashes = [];
      playing = false; act3Done = true; renderStep();
    }

    function replayCount(t) {
      const { r0, a, rmax } = rate;
      const t1 = Math.log(rmax / r0) / a;
      if (t <= t1) return r0 * (Math.exp(a * t) - 1) / a;
      return r0 * (rmax / r0 - 1) / a + rmax * (t - t1);
    }
    function replayRate(t) { return Math.min(rate.rmax, rate.r0 * Math.exp(rate.a * t)); }

    function goAct(a) {
      if (a === 1) { act = 1; playing = false; act2Done = false; act3Done = false; clock = 0; replay = null; renderStep(); }
      else if (a === 2) startAct2();
      else startAct3();
    }

    q(".mv-play").addEventListener("click", () => {
      if (act === 1) startAct2();
      else if (act === 2) (act2Done ? startAct3() : startAct2());
      else startAct3();
    });
    q(".mv-skip").addEventListener("click", () => { if (act === 2) finishAct2(); else if (act === 3) finishAct3(); });
    q(".mv-restart").addEventListener("click", () => goAct(1));
    root.querySelectorAll(".mv-steps button").forEach((b) => b.addEventListener("click", () => goAct(Number(b.parentElement.dataset.act))));
    picker.addEventListener("change", (e) => setModel(e.target.value));
    q(".mv-colour").addEventListener("change", (e) => applyColour(e.target.value));

    // -------------------------------------------------------------- drawing

    const ease = (x) => 1 - Math.pow(1 - Math.min(1, Math.max(0, x)), 3);
    const back = (x) => { x = Math.min(1, Math.max(0, x)); const c = 1.7; return 1 + (c + 1) * Math.pow(x - 1, 3) + c * Math.pow(x - 1, 2); };

    function nodeVisible(id, t) { return act === 1 ? G.depth[id] === 0 : S.appear[id] <= t; }
    function edgeAlpha(e, t) { return act === 1 ? 0 : ease((t - S.edgeAt[e.id]) / 450); }

    function curve(p, qq) {
      const dx = qq.x - p.x, dy = qq.y - p.y;
      const len = Math.hypot(dx, dy) || 1;
      const k = Math.min(18, len * 0.14);
      return { cx: (p.x + qq.x) / 2 - dy / len * k, cy: (p.y + qq.y) / 2 + dx / len * k };
    }
    function strokeEdge(e) {
      const p = L.pos[e.a], qq = L.pos[e.b], c = curve(p, qq);
      ctx.beginPath(); ctx.moveTo(p.x, p.y); ctx.quadraticCurveTo(c.cx, c.cy, qq.x, qq.y); ctx.stroke();
    }
    function pointOnEdge(e, s) {
      const p = L.pos[e.a], qq = L.pos[e.b], c = curve(p, qq);
      const u = 1 - s;
      return { x: u * u * p.x + 2 * u * s * c.cx + s * s * qq.x, y: u * u * p.y + 2 * u * s * c.cy + s * s * qq.y };
    }

    function draw(now) {
      if (!root.isConnected) return;
      requestAnimationFrame(draw);
      const dt = lastFrame ? Math.min(200, now - lastFrame) : 16;
      lastFrame = now;
      if (!G || !W) return;
      if (playing && act === 2) {
        clock += dt;
        if (clock >= S.total) finishAct2();
      }
      if (playing && act === 3) {
        replay.t += dt;
        const target = Math.min(G.paths.length, Math.floor(replayCount(replay.t)));
        while (replay.done < target) {
          const path = G.paths[replay.done];
          const r = replayRate(replay.t);
          const dur = Math.max(260, Math.min(1400, 0.85 / r));
          if (replay.flashes.length < 80) replay.flashes.push({ path, t0: replay.t, dur });
          else markVerified(path);
          replay.done++;
        }
        if (replay.done >= G.paths.length && !replay.flashes.length) finishAct3();
      }
      if (!visible) return;
      const t = clock;
      ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
      ctx.clearRect(0, 0, W, H);

      drawSectors(t);

      const hl = hover >= 0 ? hover : selected;
      const hlEdges = new Map();
      if (hl >= 0) {
        for (const e of G.edges) { if (e.a === hl) hlEdges.set(e.id, "out"); else if (e.b === hl) hlEdges.set(e.id, "in"); }
      }
      if (hoverEdge >= 0) hlEdges.set(hoverEdge, "pick");

      ctx.lineWidth = 1;
      for (const e of G.edges) {
        if (hlEdges.has(e.id)) continue;
        const a = edgeAlpha(e, t);
        if (a <= 0) continue;
        const v = replay && replay.verifiedE[e.id];
        ctx.strokeStyle = v ? rgba(theme.ok, 0.6 * a) : rgba(theme.muted, (hl >= 0 ? theme.edge * 0.4 : theme.edge) * a);
        ctx.setLineDash(e.choice ? [3, 3] : []);
        strokeEdge(e);
      }
      ctx.setLineDash([]);

      if (replay) drawFlashes();

      for (const [id, kind] of hlEdges) {
        const e = G.edges[id];
        if (edgeAlpha(e, t) <= 0) continue;
        ctx.lineWidth = kind === "pick" ? 3 : 2;
        ctx.strokeStyle = kind === "in" ? theme.blocks : theme.accent;
        ctx.setLineDash(e.choice ? [4, 3] : []);
        strokeEdge(e);
      }
      ctx.setLineDash([]);

      if (act === 2 && playing) drawWavefront(t);

      const near = new Set();
      if (hl >= 0) { for (const tr of G.model.out[hl]) near.add(tr.to); for (const tr of G.model.inn[hl]) near.add(tr.from); }
      for (let i = 0; i < G.n; i++) {
        if (!nodeVisible(i, t)) continue;
        const p = L.pos[i];
        const age = act === 1 ? 1 : (t - S.appear[i]) / 420;
        const sc = act === 1 ? 1 : back(age);
        const init = G.depth[i] === 0;
        let r = Math.max(0, L.nodeR * (init ? 1.7 : 1) * sc);
        if (i === hl) r = Math.max(r, L.nodeR * 1.7);
        ctx.globalAlpha = hl >= 0 && i !== hl && !near.has(i) ? 0.45 : 1;
        ctx.beginPath(); ctx.arc(p.x, p.y, r, 0, Math.PI * 2);
        ctx.fillStyle = colourOf[i]; ctx.fill();
        if (init) { ctx.lineWidth = 1.5; ctx.strokeStyle = theme.fg; ctx.stroke(); }
        if (replay && replay.verifiedN[i] && !init) { ctx.lineWidth = 1; ctx.strokeStyle = rgba(theme.ok, 0.8); ctx.stroke(); }
        ctx.globalAlpha = 1;
        if (i === selected) { ctx.beginPath(); ctx.arc(p.x, p.y, r + 4, 0, Math.PI * 2); ctx.lineWidth = 2.5; ctx.strokeStyle = theme.accent; ctx.stroke(); }
        if (i === hover) { ctx.beginPath(); ctx.arc(p.x, p.y, r + 3, 0, Math.PI * 2); ctx.lineWidth = 1.5; ctx.strokeStyle = theme.fg; ctx.stroke(); }
      }

      if (act === 1) drawStartLabels();
      updateCounters(t);
    }

    function markVerified(path) {
      for (const tr of path) { const e = G.edgeOfT[tr.id]; if (e >= 0) replay.verifiedE[e] = 1; replay.verifiedN[tr.to] = 1; replay.verifiedN[tr.from] = 1; }
    }

    function drawFlashes() {
      const keep = [];
      for (const f of replay.flashes) {
        const age = replay.t - f.t0;
        const head = Math.min(1, age / f.dur);
        const fade = age > f.dur ? 1 - Math.min(1, (age - f.dur) / 450) : 1;
        if (fade <= 0) { markVerified(f.path); continue; }
        keep.push(f);
        const n = f.path.length;
        ctx.lineCap = "round";
        for (let k = 0; k < n; k++) {
          const tr = f.path[k];
          const eid = G.edgeOfT[tr.id];
          const seg0 = k / n, seg1 = (k + 1) / n;
          if (head <= seg0) break;
          const frac = Math.min(1, (head - seg0) / (seg1 - seg0));
          if (eid < 0) {
            if (head >= seg1 || frac > 0.3) {
              const p = L.pos[tr.to];
              ctx.beginPath(); ctx.arc(p.x, p.y, L.nodeR + 2 + 6 * frac, 0, Math.PI * 2);
              ctx.lineWidth = 2; ctx.strokeStyle = rgba(theme.accent, 0.9 * fade * (1 - 0.6 * frac)); ctx.stroke();
            }
            continue;
          }
          const e = G.edges[eid];
          ctx.lineWidth = 2.5; ctx.strokeStyle = rgba(theme.accent, 0.9 * fade);
          ctx.setLineDash(e.choice ? [4, 3] : []);
          if (frac >= 1) strokeEdge(e);
          else {
            const steps = 8;
            ctx.beginPath();
            for (let s = 0; s <= steps; s++) { const pt = pointOnEdge(e, frac * s / steps); if (s === 0) ctx.moveTo(pt.x, pt.y); else ctx.lineTo(pt.x, pt.y); }
            ctx.stroke();
            const hp = pointOnEdge(e, frac);
            ctx.setLineDash([]);
            ctx.beginPath(); ctx.arc(hp.x, hp.y, 4, 0, Math.PI * 2); ctx.fillStyle = rgba(theme.accent, fade); ctx.fill();
          }
          ctx.setLineDash([]);
          if (frac >= 1 && k === n - 1) {
            const p = L.pos[tr.to];
            ctx.beginPath(); ctx.arc(p.x, p.y, L.nodeR + 3, 0, Math.PI * 2); ctx.lineWidth = 2; ctx.strokeStyle = rgba(theme.ok, fade); ctx.stroke();
          }
        }
        ctx.lineCap = "butt";
      }
      replay.flashes = keep;
    }

    function drawWavefront(t) {
      const d = S.ringStart.findIndex((r) => t < r.t + r.dur);
      if (d < 0) return;
      const r = S.ringStart[d];
      const frac = Math.max(0, (t - r.t - 250) / (r.dur - 250));
      const rad = d + L.base + frac;
      ctx.beginPath();
      if (!L.multi) ctx.ellipse(L.cx, L.cy, L.unitX * rad, L.unitY * rad, 0, 0, Math.PI * 2);
      else {
        for (const a of G.model.aspects) {
          const s = L.sectors[a];
          ctx.moveTo(L.cx + Math.cos(s.start) * L.unitX * rad, L.cy + Math.sin(s.start) * L.unitY * rad);
          ctx.ellipse(L.cx, L.cy, L.unitX * rad, L.unitY * rad, 0, s.start, s.start + s.span);
        }
      }
      ctx.lineWidth = 10; ctx.strokeStyle = rgba(theme.accent, 0.08); ctx.stroke();
      ctx.lineWidth = 1.5; ctx.strokeStyle = rgba(theme.accent, 0.45); ctx.stroke();
    }

    function drawSectors(t) {
      if (!L.multi) return;
      const outer = G.maxDepth + L.base + 0.55;
      ctx.font = "600 12px " + getComputedStyle(root).fontFamily;
      ctx.textBaseline = "middle";
      for (const a of G.model.aspects) {
        const s = L.sectors[a];
        const anyVisible = act > 1 || G.model.initial.some((id) => G.model.cores[id].fields.aspect === a);
        ctx.globalAlpha = anyVisible ? 1 : 0.5;
        const x = L.cx + Math.cos(s.mid) * L.unitX * outer, y = L.cy + Math.sin(s.mid) * L.unitY * outer;
        ctx.fillStyle = colourBy === "aspect" ? aspectColour(a) : theme.muted;
        ctx.textAlign = Math.cos(s.mid) > 0.3 ? "left" : Math.cos(s.mid) < -0.3 ? "right" : "center";
        const tw = ctx.measureText(a).width;
        const lx = ctx.textAlign === "left" ? Math.min(x, W - tw - 10) : ctx.textAlign === "right" ? Math.max(x, tw + 10) : Math.min(Math.max(x, tw / 2 + 10), W - tw / 2 - 10);
        ctx.fillText(a, lx, Math.min(Math.max(y, lx < 520 ? 84 : 14), H - 72));
        ctx.strokeStyle = rgba(theme.border, 0.9); ctx.lineWidth = 1; ctx.setLineDash([2, 5]);
        const e0 = s.start - 0.08;
        ctx.beginPath(); ctx.moveTo(L.cx + Math.cos(e0) * L.unitX * 0.25, L.cy + Math.sin(e0) * L.unitY * 0.25); ctx.lineTo(L.cx + Math.cos(e0) * L.unitX * outer, L.cy + Math.sin(e0) * L.unitY * outer); ctx.stroke();
        ctx.setLineDash([]);
        ctx.globalAlpha = 1;
      }
    }

    function drawStartLabels() {
      ctx.font = "12px " + getComputedStyle(root).fontFamily;
      ctx.fillStyle = theme.muted;
      for (const id of G.model.initial) {
        const p = L.pos[id];
        if (!L.multi) { ctx.textAlign = "center"; ctx.textBaseline = "top"; ctx.fillText("start", p.x, p.y + L.nodeR * 1.7 + 6); continue; }
        if (G.model.initial.length > 6) continue;
        const a = L.sectors[G.model.cores[id].fields.aspect].mid;
        ctx.textAlign = Math.cos(a) > 0.3 ? "left" : Math.cos(a) < -0.3 ? "right" : "center";
        ctx.textBaseline = Math.abs(Math.cos(a)) > 0.3 ? "middle" : Math.sin(a) > 0 ? "top" : "bottom";
        ctx.fillText(`start of ${G.model.cores[id].fields.aspect}`, p.x + Math.cos(a) * (L.nodeR * 1.7 + 8), p.y + Math.sin(a) * (L.nodeR * 1.7 + 8));
      }
    }

    function updateCounters(t) {
      let states, trans;
      if (act === 1) { states = G.model.initial.length; trans = 0; }
      else {
        states = 0; for (const a of S.appearSorted) { if (a <= t) states++; else break; }
        trans = 0; for (const id of S.transByExpand) { if (S.expand[id] <= t) trans += G.model.out[id].length; else break; }
      }
      setCounter(".mv-c-states", states, act === 2 && playing, act >= 2 && states === G.n);
      setCounter(".mv-c-trans", trans, act === 2 && playing, act >= 2 && trans === G.model.transitions.length);
      setCounter(".mv-c-paths", replay ? replay.done : 0, act === 3 && playing, act3Done);
    }
    function setCounter(sel, v, live, ok) {
      const el = q(sel);
      const b = el.firstElementChild;
      if (b.textContent !== String(v)) b.textContent = v;
      el.classList.toggle("live", live);
      el.classList.toggle("ok", ok && !live);
    }

    // ----------------------------------------------------------- interaction

    function nodeAt(x, y) {
      let best = -1, bd = 14 * 14;
      for (let i = 0; i < G.n; i++) {
        if (!nodeVisible(i, clock)) continue;
        const p = L.pos[i];
        const d = (p.x - x) ** 2 + (p.y - y) ** 2;
        if (d < bd) { bd = d; best = i; }
      }
      return best;
    }
    canvas.addEventListener("mousemove", (e) => {
      if (!G) return;
      const r = canvas.getBoundingClientRect();
      const x = e.clientX - r.left, y = e.clientY - r.top;
      hover = nodeAt(x, y);
      if (hover >= 0) {
        const c = G.model.cores[hover];
        tip.hidden = false;
        tip.innerHTML = `<b>state ${hover}</b> · ${esc(c.fields.aspect)} · ${G.depth[hover] === 0 ? "starting state" : `${G.depth[hover]} call${G.depth[hover] > 1 ? "s" : ""} from the start`} · ${G.model.out[hover].length} calls out`;
        tip.style.left = `${Math.max(4, Math.min(x + 14, W - tip.offsetWidth - 8))}px`;
        tip.style.top = `${y + 16}px`;
      } else tip.hidden = true;
      canvas.style.cursor = hover >= 0 ? "pointer" : "crosshair";
    });
    canvas.addEventListener("mouseleave", () => { hover = -1; tip.hidden = true; });
    canvas.addEventListener("click", () => { if (!G) return; selected = hover; hoverEdge = -1; renderDetail(); });

    function renderDetail() {
      const box = q(".mv-detail");
      if (selected < 0) { box.innerHTML = EMPTY_DETAIL; return; }
      const m = G.model, id = selected;
      const sc = RX.scene(m, id);
      const d = G.depth[id];
      let h = `<h3>State ${id} <span class="rx-badge" style="color:${aspectColour(sc.aspect)}">${esc(sc.aspect)}</span></h3>`;
      h += `<p class="sub">${d === 0 ? "A starting state." : `Reached after ${d} call${d > 1 ? "s" : ""}.`} ${m.inn[id].filter((t) => t.from !== t.to).length} lines in, ${m.out[id].filter((t) => t.from !== t.to).length} out, ${m.out[id].filter((t) => t.from === t.to).length} calls that change nothing.</p>`;
      h += `<div class="rooms">`;
      for (const r of sc.ractors) {
        const threads = sc.threads.filter((th) => th.ractor === r.id);
        const ports = sc.ports.filter((p) => p.owner === r.id);
        h += `<div class="room rx-ractor ${r.alive ? r.color : "dead"}"><div class="room-head"><span>${esc(r.name)}</span><span class="state">${r.alive ? "alive" : "terminated"}</span></div><div class="room-body">`;
        for (const p of ports) {
          h += `<span class="slot ${p.open ? "" : "closed"}" title="${esc(p.note || "")}">${esc(p.label)}${p.open ? "" : '<i class="lock"></i>'}${p.messages.length ? ` <i class="env"></i><span class="count">×${p.messages.length}</span>` : ""}</span>`;
        }
        for (const th of threads) {
          h += `<span class="thr ${th.status}"><i></i>${esc(th.label)}${th.waiting ? ` <span class="wait">${esc(th.waiting)}</span>` : ""}${th.unwound ? ` <span class="wait">ensure ran</span>` : ""}</span>`;
        }
        h += `</div></div>`;
      }
      h += `</div>`;
      if (sc.facts.length) {
        h += `<h4>Facts</h4><dl class="facts">${sc.facts.map((f) => `<dt>${esc(f.label)}</dt><dd>${esc(f.value)}</dd>`).join("")}</dl>`;
      }
      const outs = m.out[id];
      h += `<h4>Calls from here (${outs.length})</h4><ul class="calls">`;
      for (const t of outs) {
        const dsc = RX.describe(m, t);
        const self = t.to === id;
        h += `<li data-t="${t.id}"><span class="rx-badge ${dsc.kind}">${esc(t.outcome.replace(/^Ractor::/, ""))}</span><span class="title">${esc(dsc.title)}</span>${t.choice ? '<span class="ch">choice</span>' : ""}<span class="to ${self ? "self" : ""}">${self ? "stays" : `→ ${t.to}`}</span></li>`;
      }
      h += `</ul>`;
      box.innerHTML = h;
      box.querySelectorAll(".calls li").forEach((li) => {
        const t = m.transitions[Number(li.dataset.t)];
        li.addEventListener("mouseenter", () => { hoverEdge = G.edgeOfT[t.id]; });
        li.addEventListener("mouseleave", () => { hoverEdge = -1; });
        li.addEventListener("click", () => { if (t.to !== id && nodeVisible(t.to, clock)) { selected = t.to; hoverEdge = -1; renderDetail(); } });
      });
    }

    // ----------------------------------------------------------------- boot

    readTheme();
    darkMedia.addEventListener("change", () => requestAnimationFrame(readTheme));
    new ResizeObserver(resize).observe(wrap);
    if ("IntersectionObserver" in window) new IntersectionObserver((es) => { visible = es.some((e) => e.isIntersecting); }).observe(wrap);
    resize();
    setModel(picker.value);
    requestAnimationFrame(draw);
  }

  window.RXWidgets = window.RXWidgets || {};
  window.RXWidgets.multiverse = { mount };
})();
