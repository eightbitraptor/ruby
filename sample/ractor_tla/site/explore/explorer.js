// Explorer widget: the user picks the next call and watches it happen.
//
//   <div data-rx-widget="explorer" data-urlparams></div>
//
// Options: model (default: first supported), aspect, urlparams (read ?model= and
// ?aspect= at mount and keep them updated).
(function () {
  "use strict";

  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };

  function mount(root, options = {}) {
    root.classList.add("rx-explorer");
    const names = RX.supported();
    const MODELS = Object.fromEntries(names.map((n) => [n, RX.load(n)]));
    const visited = Object.fromEntries(names.map((n) => [n, new Set()]));
    const useUrl = options.urlparams != null;
    const state = { model: null, aspect: null, start: 0, id: 0, history: [], busy: false, guess: true };

    const modelSel = el("select", "rx-select");
    const aspectSel = el("select", "rx-select");
    const guessBox = el("input");
    guessBox.type = "checkbox";
    guessBox.checked = true;
    const resetBtn = el("button", "rx-btn", "⟲ Start over");
    const pickers = el("div", "ex-pickers rx-panel");
    const lab = (text, ctl, cls) => { const l = el("label", cls); l.append(document.createTextNode(text + " "), ctl); return l; };
    const guessLabel = el("label", "ex-toggle");
    guessLabel.append(guessBox, document.createTextNode(" Guess first"));
    pickers.append(lab("Model", modelSel), lab("Scenario", aspectSel), guessLabel, resetBtn);

    const stageEl = el("div", "ex-stage");
    const stageWrap = el("div", "ex-stagewrap rx-panel");
    stageWrap.append(stageEl);
    const meterBar = el("i");
    const meterText = el("p", "ex-metertext");
    const meter = el("div", "ex-meter rx-panel");
    const bar = el("div", "bar");
    bar.append(meterBar);
    meter.append(bar, meterText);
    const undoBtn = el("button", "rx-btn", "↶ Undo");
    const histList = el("ol", "ex-histlist");
    const histPanel = el("section", "ex-history rx-panel");
    const hh = el("div", "hh");
    hh.append(el("div", "ex-h", "Your path"), undoBtn);
    histPanel.append(hh, histList);
    const left = el("section", "ex-left");
    left.append(stageWrap, meter, histPanel);

    const hint = el("p", "rx-muted hint");
    const actions = el("div", "ex-actlist");
    const aside = el("aside", "ex-actions rx-panel");
    aside.append(el("div", "ex-h", "What happens next?"), hint, actions);
    const body = el("div", "ex-body");
    body.append(left, aside);

    const pick = el("div", "ex-pick");
    pick.hidden = true;
    const pickBox = el("div", "box rx-panel");
    const pickTitle = el("div", "ex-h");
    const pickOpts = el("div", "ex-pickopts");
    const pickClose = el("button", "rx-btn", "Cancel");
    pickBox.append(pickTitle, el("p", "rx-muted", "The specification allows more than one result for this call. CRuby takes one of them each time. Pick the one you want to watch."), pickOpts, pickClose);
    pick.append(pickBox);

    root.append(pickers, body, pick);
    const stage = RXScene.mount(stageEl);

    const badgeText = (t) => {
      const k = RX.kind(t.outcome);
      return { ok: "ok", nil: "nil", blocks: "waits", runs: "runs", killed: "killed", exit: "exit" }[k] || RXScene.exceptionName(t.outcome);
    };
    const badge = (t) => el("span", `rx-badge ${RX.kind(t.outcome)}`, badgeText(t));

    function actorInfo(t) {
      const a = RXScene.actorOf(state.model, t);
      if (a.thread != null) {
        const th = RX.CAST[state.model.name].threads[a.thread];
        return { key: `t${a.thread}`, label: RX.threadName(state.model, a.thread), ractor: th ? th.ractor : 1 };
      }
      return { key: `r${a.ractor}`, label: RX.ractorName(state.model, a.ractor), ractor: a.ractor };
    }

    const aspectsOf = (model) => model.initial.map((id) => model.byId[id].fields.aspect).filter((a, i, arr) => arr.indexOf(a) === i);

    function fillPickers() {
      modelSel.replaceChildren(...names.map((n) => { const o = el("option", null, `${n} — ${MODELS[n].summary}`); o.value = n; return o; }));
      modelSel.value = state.model.name;
      const aspects = aspectsOf(state.model);
      aspectSel.replaceChildren(...aspects.map((a) => { const o = el("option", null, a); o.value = a; return o; }));
      aspectSel.value = state.aspect;
      aspectSel.disabled = aspects.length < 2;
    }

    function syncUrl() {
      if (!useUrl) return;
      const u = new URL(location.href);
      u.searchParams.set("model", state.model.name);
      if (aspectsOf(state.model).length > 1) u.searchParams.set("aspect", state.aspect);
      else u.searchParams.delete("aspect");
      window.history.replaceState(null, "", u);
    }

    function begin(modelName, aspect) {
      state.model = MODELS[modelName] || MODELS[names[0]];
      const aspects = aspectsOf(state.model);
      state.aspect = aspect && aspects.includes(aspect) ? aspect : aspects[0];
      state.start = state.model.initial.find((id) => state.model.byId[id].fields.aspect === state.aspect);
      state.id = state.start;
      state.history = [];
      visited[state.model.name].add(state.id);
      fillPickers();
      syncUrl();
      stage.render(RX.scene(state.model, state.id));
      stage.caption(aspects.length > 1 ? `Start of the "${state.aspect}" scenario. Pick a call on the right.` : "Start state. Pick a call on the right.");
      refresh();
    }

    function groups() {
      const map = new Map();
      for (const t of state.model.out[state.id]) {
        const key = t.op + JSON.stringify(t.args);
        if (!map.has(key)) map.set(key, []);
        map.get(key).push(t);
      }
      return [...map.values()];
    }

    function refresh() {
      const guess = guessBox.checked;
      hint.textContent = guess ? "Guess the result before you click. The badge appears when the call runs." : "Each badge shows how the call ends.";
      actions.replaceChildren();
      const byActor = new Map();
      for (const g of groups()) {
        const info = actorInfo(g[0]);
        if (!byActor.has(info.key)) byActor.set(info.key, { info, groups: [] });
        byActor.get(info.key).groups.push(g);
      }
      const sorted = [...byActor.values()].sort((a, b) => a.info.ractor - b.info.ractor || a.info.key.localeCompare(b.info.key));
      if (!sorted.length) actions.append(el("p", "ex-none", "Nothing can happen from here. This is an end state."));
      for (const { info, groups: gs } of sorted) {
        const sec = el("div", `ex-group rx-ractor ${RX.COLORS[info.ractor] || "main"}`);
        const h = el("div", "ex-gname");
        h.append(el("i"), document.createTextNode(info.label));
        sec.append(h);
        for (const g of gs) {
          const t = g[0];
          const d = RX.describe(state.model, t);
          const b = el("button", "ex-act" + (!guess && g.every((x) => x.from === x.to) ? " loop" : ""));
          b.append(el("span", "t", d.title));
          if (g.length > 1) b.append(el("span", "rx-badge choice", "CRuby decides"));
          else if (guess) b.append(el("span", "rx-badge hidden", "?"));
          else b.append(badge(t));
          b.title = d.ruby;
          b.disabled = state.busy;
          b.addEventListener("click", () => (g.length > 1 ? choose(g, d) : go(t)));
          sec.append(b);
        }
        actions.append(sec);
      }
      histList.replaceChildren();
      if (!state.history.length) histList.append(el("li", "empty", "No calls yet."));
      state.history.forEach((t) => {
        const li = el("li", null, RX.describe(state.model, t).title);
        li.append(badge(t));
        histList.append(li);
      });
      histList.scrollTop = histList.scrollHeight;
      undoBtn.disabled = !state.history.length || state.busy;
      const n = state.model.cores.length, x = visited[state.model.name].size;
      meterBar.style.width = `${(100 * x) / n}%`;
      meterText.replaceChildren(
        document.createTextNode(`You visited ${x} of ${n} states. TLC visited all ${n}. `),
        el("span", null, `CRuby replayed ${state.model.counts.paths} paths of this model; all passed.`)
      );
    }

    function choose(group, d) {
      pickTitle.textContent = d.title;
      pickOpts.replaceChildren();
      for (const t of group) {
        const dd = RX.describe(state.model, t);
        const b = el("button", "ex-opt");
        b.append(badge(t), el("span", "s", dd.sentences.filter((s) => !s.startsWith("This is a choice")).join(" ")));
        b.addEventListener("click", () => { pick.hidden = true; go(t); });
        pickOpts.append(b);
      }
      pick.hidden = false;
      pickClose.focus();
    }

    async function go(t) {
      if (state.busy) return;
      state.busy = true;
      refresh();
      stage.caption(RX.describe(state.model, t));
      try { await stage.play(state.model, t); } finally { state.busy = false; }
      state.id = t.to;
      state.history.push(t);
      visited[state.model.name].add(t.to);
      refresh();
    }

    function undo() {
      if (state.busy || !state.history.length) return;
      const t = state.history.pop();
      state.id = state.history.length ? state.history[state.history.length - 1].to : state.start;
      stage.render(RX.scene(state.model, state.id));
      stage.caption(`Undid "${RX.describe(state.model, t).title}".`);
      refresh();
    }

    modelSel.addEventListener("change", () => begin(modelSel.value, null));
    aspectSel.addEventListener("change", () => begin(state.model.name, aspectSel.value));
    guessBox.addEventListener("change", refresh);
    resetBtn.addEventListener("click", () => { visited[state.model.name].clear(); begin(state.model.name, state.aspect); });
    undoBtn.addEventListener("click", undo);
    pickClose.addEventListener("click", () => { pick.hidden = true; });
    pick.addEventListener("click", (e) => { if (e.target === pick) pick.hidden = true; });
    root.addEventListener("keydown", (e) => {
      if (e.key === "Escape") pick.hidden = true;
      if (e.key === "z" && (e.ctrlKey || e.metaKey)) { e.preventDefault(); undo(); }
    });

    const params = useUrl ? new URL(location.href).searchParams : null;
    begin((params && params.get("model")) || options.model || names[0], (params && params.get("aspect")) || options.aspect || null);
  }

  window.RXWidgets = window.RXWidgets || {};
  window.RXWidgets.explorer = { mount };
})();
