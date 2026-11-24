// Theatre widget: plays curated highlights and outcome examples on an animated stage.
//
//   <div data-rx-widget="theatre" data-model="RactorPorts" data-calls="#calls" data-lifecycles="#lifecycles" data-dock></div>
//   <div data-rx-widget="theatre" data-scope="all"></div>
//
// Options: model, story (story id), scope ("all" = gallery), calls (anchor of the Calls
// section), lifecycles (anchor of the lifecycle diagrams), dock (allow a fixed panel).
//
// Any <button data-rx-play data-model="M" data-path="12,40,41" data-label="…"> on the page
// plays that transition path in the theatre of model M.  Every shown state is announced
// with document event "rx:state" {model, state, prev, transition}.
(function () {
  "use strict";

  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };

  const instances = new Map();
  let delegated = false;

  function installDelegate() {
    if (delegated) return;
    delegated = true;
    document.addEventListener("click", (e) => {
      const btn = e.target.closest("[data-rx-play]");
      if (!btn) return;
      const inst = instances.get(btn.dataset.model);
      if (!inst) return;
      e.preventDefault();
      inst.playIds(btn.dataset.path, btn.dataset.label || "Example");
    });
  }

  function announce(model, state, prev, transition) {
    document.dispatchEvent(new CustomEvent("rx:state", { detail: { model: model.name, state, prev, transition } }));
  }

  function scopeText(model) {
    const stories = RX.stories(model);
    const s = RX.scope(model, stories.map((x) => x.path));
    return `${stories.length === 1 ? "This highlight shows" : `These ${stories.length} highlights show`} ${s.shown} of the ${s.total} outcomes of this model.`;
  }

  function mount(root, options = {}) {
    root.classList.add("rx-theatre");
    const gallery = options.scope === "all";
    const models = gallery ? RX.supported().map((n) => RX.load(n)) : [RX.load(options.model)];
    const entries = [];
    models.forEach((m) => RX.stories(m).forEach((st) => entries.push({ model: m, story: st })));
    if (!entries.length) { root.hidden = true; return; }
    root.classList.add(gallery ? "gallery" : "compact");
    const canDock = !gallery && options.dock != null;

    const stageEl = el("div", "th-stage");
    const stageWrap = el("div", "th-stagewrap rx-panel");
    stageWrap.append(stageEl);
    const modelBox = el("div", "th-model rx-panel");
    const endEl = el("div", "th-end");
    endEl.hidden = true;
    const dotsEl = el("div", "th-dots");
    const buttons = el("div", "th-buttons");
    const restartBtn = el("button", "rx-btn", "⟲ Restart");
    const backBtn = el("button", "rx-btn", "◀ Back");
    const playBtn = el("button", "rx-btn primary", "▶ Play");
    const fwdBtn = el("button", "rx-btn", "Next ▶");
    const speedBox = el("span", "th-speed");
    [0.5, 1, 2].forEach((s) => {
      const b = el("button", "rx-btn" + (s === 1 ? " on" : ""), `${s}×`);
      b.addEventListener("click", () => {
        state.speed = s;
        stage.speed = s;
        speedBox.querySelectorAll(".rx-btn").forEach((x) => x.classList.toggle("on", x === b));
      });
      speedBox.append(b);
    });
    buttons.append(restartBtn, backBtn, playBtn, fwdBtn, speedBox);
    const keepBtn = el("button", "rx-btn th-keep", "Keep in view");
    if (canDock) buttons.append(keepBtn);
    const controls = el("div", "th-controls rx-panel");
    controls.append(dotsEl, buttons);
    const legend = el("p", "th-legend rx-muted");
    [["idle", "idle thread"], ["blocked", "waiting thread"], ["running", "running thread"], ["dead", "ended thread"], ["env", "message in a port"], ["lock", "closed port"]]
      .forEach(([c, t]) => { const s = el("span", "lg"); s.append(el("i", c), document.createTextNode(" " + t)); legend.append(s); });

    const picker = el(gallery ? "nav" : "div", gallery ? "th-stories" : "th-chips");
    const why = el("p", "th-why rx-muted");
    const dockbar = el("div", "th-dockbar");
    const dockTitle = el("span", "ttl");
    const backHl = el("button", "rx-btn", "Back to highlights");
    const closeDock = el("button", "rx-btn", "Close");
    dockbar.append(dockTitle, backHl, closeDock);
    const body = el("div", "th-body");
    const placeholder = el("div", "th-placeholder");
    if (gallery) {
      body.append(stageWrap, modelBox, endEl, controls, legend);
      const main = el("section", "th-main");
      main.append(body);
      const side = el("aside", "th-side");
      side.append(picker);
      root.append(side, main);
    } else {
      const head = el("div", "th-head");
      head.append(el("span", "rx-kicker", "Highlights"));
      const scope = el("p", "th-scope rx-muted", scopeText(models[0]) + " ");
      if (options.calls) {
        const a = el("a", null, "Calls");
        a.href = options.calls;
        scope.append(document.createTextNode("Every outcome has its own example under "), a, document.createTextNode("; press ▶ Play there to watch it."));
      }
      head.append(scope);
      body.append(dockbar, picker, why, stageWrap, modelBox, endEl, controls);
      root.append(head, placeholder, body);
    }

    const stage = RXScene.mount(stageEl);
    const state = { model: null, story: null, idx: 0, playing: false, busy: false, run: 0, speed: 1, highlight: null, docked: false };

    function buildPicker() {
      picker.replaceChildren();
      if (gallery) {
        models.forEach((m) => {
          const stories = RX.stories(m);
          if (!stories.length) return;
          const g = el("div", "th-group");
          g.append(el("div", "th-gname", m.name), el("p", "sum", m.summary), el("p", "sum", scopeText(m)));
          stories.forEach((st, i) => {
            const b = el("button", "th-story");
            b.dataset.model = m.name;
            b.dataset.story = st.id;
            b.append(el("span", "n", String(i + 1)), el("span", "t", st.title), el("span", "steps", `${st.path.length} step${st.path.length > 1 ? "s" : ""}`));
            b.addEventListener("click", () => select(m, st));
            g.append(b);
          });
          picker.append(g);
        });
      } else {
        entries.forEach(({ model, story }) => {
          const b = el("button", "th-chip", story.title);
          b.dataset.story = story.id;
          b.addEventListener("click", () => select(model, story));
          picker.append(b);
        });
      }
    }

    function markPicker() {
      picker.querySelectorAll(".th-story, .th-chip").forEach((b) =>
        b.classList.toggle("on", (!b.dataset.model || b.dataset.model === state.model.name) && b.dataset.story === state.story.id));
    }

    const stateAt = (i) => (i === 0 ? state.story.path[0].from : state.story.path[i - 1].to);

    function drawDots() {
      const n = state.story.path.length;
      dotsEl.replaceChildren();
      for (let i = 0; i <= n; i++) {
        const d = el("button", "dot" + (i === 0 ? " start" : "") + (i <= state.idx && i > 0 ? " done" : "") + (i === state.idx ? " now" : ""));
        d.title = i === 0 ? "start" : RX.describe(state.model, state.story.path[i - 1]).title;
        d.addEventListener("click", () => { pause(); showStep(i); });
        dotsEl.append(d);
      }
      dotsEl.append(el("span", "lbl", state.idx === 0 ? `${n} step${n > 1 ? "s" : ""}` : `step ${state.idx} of ${n}`));
      backBtn.disabled = state.idx === 0;
      fwdBtn.disabled = state.idx >= n;
      playBtn.disabled = state.idx >= n;
    }

    function code(text) { return el("code", "th-code", text); }

    function describeModel(i) {
      const m = state.model;
      const multi = m.aspects.length > 1;
      modelBox.replaceChildren(el("span", "rx-kicker", "In the model"));
      const lines = el("div", "th-lines");
      modelBox.append(lines);
      if (i === 0) {
        const aspect = m.byId[stateAt(0)].fields.aspect;
        const p = el("div", "th-line");
        p.append(document.createTextNode(multi ? "Start state of scenario " : "Start state."));
        if (multi) p.append(code(aspect), document.createTextNode("."));
        if (options.lifecycles) {
          const a = el("a", null, "The lifecycle diagrams mark this state.");
          a.href = options.lifecycles;
          p.append(document.createTextNode(" "), a);
        }
        lines.append(p);
        return;
      }
      const t = state.story.path[i - 1];
      const head = el("div", "th-line");
      head.append(document.createTextNode("Action "));
      const def = document.getElementById("def-" + t.op);
      if (def) {
        const a = el("a");
        a.href = "#def-" + t.op;
        a.append(code(t.op));
        head.append(a);
      } else head.append(code(t.op));
      if (multi) head.append(document.createTextNode(" · scenario "), code(m.byId[t.from].fields.aspect));
      lines.append(head);
      const changes = RX.diff(m, t);
      if (!changes.length) lines.append(el("div", "th-line rx-muted", "No variable changes: the call returns or raises and leaves the state as it was."));
      for (const c of changes) {
        const row = el("div", "th-var");
        row.append(code(c.name));
        if (c.hint) row.append(el("span", "rx-muted hint", c.hint));
        const v = el("span", "vals");
        v.append(code(c.before), el("span", "arr", "→"), code(c.after));
        row.append(v);
        lines.append(row);
      }
    }

    const startCaption = () => ({ title: state.story.title, kind: null, ruby: "", sentences: ["Press Play to watch the whole story, or Next for one step at a time."] });

    function showStep(i) {
      state.idx = i;
      endEl.hidden = true;
      stage.render(RX.scene(state.model, stateAt(i)));
      stage.caption(i === 0 ? startCaption() : RX.describe(state.model, state.story.path[i - 1]));
      describeModel(i);
      drawDots();
      announce(state.model, stateAt(i), null, null);
      if (i === state.story.path.length) showEnd();
    }

    function showEnd() {
      const m = state.model, st = state.story;
      endEl.replaceChildren();
      const card = el("div", "card");
      card.append(el("div", "ttl", st.title), el("p", "why", st.why),
        el("p", "checked", `TLC checked all ${m.counts.core_states} states of this model. CRuby replayed ${m.counts.paths} paths; all passed.`));
      const acts = el("div", "acts");
      const again = el("button", "rx-btn", "⟲ Replay");
      again.addEventListener("click", () => { restart(); startPlaying(); });
      acts.append(again);
      if (entries.length > 1) {
        const next = el("button", "rx-btn primary", st.example ? "Back to highlights" : "Next story ▶");
        next.addEventListener("click", st.example ? backToHighlights : nextStory);
        acts.append(next);
      }
      card.append(acts);
      endEl.append(card);
      endEl.hidden = false;
    }

    function nextStory() {
      const i = entries.findIndex((e) => e.model === state.model && e.story.id === state.story.id);
      const e = entries[(i + 1) % entries.length];
      select(e.model, e.story);
      startPlaying();
    }

    function select(model, story) {
      pause();
      state.model = model;
      state.story = story;
      if (!story.example) state.highlight = { model, story };
      markPicker();
      if (!gallery) why.textContent = story.why;
      dockTitle.textContent = `${story.example ? "Example" : "Highlight"}: ${story.title}`;
      showStep(0);
    }

    function pathFromIds(model, ids) {
      const path = ids.split(",").map((s) => model.transitions[Number(s.trim())]).filter(Boolean);
      for (let i = 1; i < path.length; i++) {
        if (path[i].from !== path[i - 1].to) console.error(`rx-play: step ${i} of path "${ids}" starts at state ${path[i].from}, but the previous step ends at ${path[i - 1].to}`);
      }
      return path;
    }

    function playIds(ids, label) {
      const model = models[0];
      const path = pathFromIds(model, ids);
      if (!path.length) { console.error(`rx-play: no transitions in path "${ids}"`); return; }
      select(model, { id: `example:${ids}`, title: label, why: "The shortest call sequence from a start state that produces this outcome.", path, example: true });
      if (canDock) dock();
      else root.scrollIntoView({ behavior: "smooth", block: "center" });
      startPlaying();
    }

    function backToHighlights() {
      undock();
      const h = state.highlight || entries[0];
      select(h.model, h.story);
    }

    function dock() {
      if (state.docked || !canDock) return;
      placeholder.style.height = `${body.offsetHeight}px`;
      state.docked = true;
      root.classList.add("docked");
      stage.render(RX.scene(state.model, stateAt(state.idx)));
    }

    function undock() {
      if (!state.docked) return;
      state.docked = false;
      root.classList.remove("docked");
      placeholder.style.height = "";
      stage.render(RX.scene(state.model, stateAt(state.idx)));
    }

    async function stepForward() {
      const n = state.story.path.length;
      if (state.busy || state.idx >= n) return;
      state.busy = true;
      endEl.hidden = true;
      const t = state.story.path[state.idx];
      stage.caption(RX.describe(state.model, t));
      describeModel(state.idx + 1);
      announce(state.model, t.to, t.from, t);
      try { await stage.play(state.model, t); } finally { state.busy = false; }
      state.idx++;
      drawDots();
      if (state.idx === n) showEnd();
    }

    function stepBack() {
      if (state.busy) return;
      pause();
      showStep(Math.max(0, state.idx - 1));
    }

    function restart() {
      pause();
      showStep(0);
    }

    function pause() {
      state.playing = false;
      state.run++;
      playBtn.textContent = "▶ Play";
    }

    async function startPlaying() {
      if (state.playing) return;
      if (state.idx >= state.story.path.length) showStep(0);
      state.playing = true;
      const run = ++state.run;
      playBtn.textContent = "❚❚ Pause";
      while (state.playing && run === state.run && state.idx < state.story.path.length) {
        const began = performance.now();
        await stepForward();
        if (run !== state.run) return;
        const gap = Math.max(2500 / state.speed - (performance.now() - began), 500 / state.speed);
        await new Promise((r) => setTimeout(r, gap));
      }
      if (run === state.run) pause();
    }

    playBtn.addEventListener("click", () => (state.playing ? pause() : startPlaying()));
    fwdBtn.addEventListener("click", () => { pause(); stepForward(); });
    backBtn.addEventListener("click", stepBack);
    restartBtn.addEventListener("click", restart);
    keepBtn.addEventListener("click", dock);
    backHl.addEventListener("click", backToHighlights);
    closeDock.addEventListener("click", undock);
    root.addEventListener("keydown", (e) => {
      if (e.key === "ArrowRight") { e.preventDefault(); pause(); stepForward(); }
      else if (e.key === "ArrowLeft") { e.preventDefault(); stepBack(); }
      else if (e.key === "Escape" && state.docked) { e.preventDefault(); undock(); }
    });

    buildPicker();
    const wanted = options.model || (gallery ? "RactorBlocking" : null);
    const first = entries.find((e) => (!wanted || e.model.name === wanted) && (!options.story || e.story.id === options.story))
      || entries.find((e) => !wanted || e.model.name === wanted) || entries[0];
    select(first.model, first.story);

    if (!gallery) {
      instances.set(models[0].name, { playIds, dock, undock });
      installDelegate();
    }
  }

  window.RXWidgets = window.RXWidgets || {};
  window.RXWidgets.theatre = { mount };
})();
