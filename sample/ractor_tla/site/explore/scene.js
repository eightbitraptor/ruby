// Animated scene renderer.
//
//   const stage = RXScene.mount(element, { speed: 1 });
//   stage.render(RX.scene(model, stateId));      draw a state at once
//   await stage.play(model, transition);         animate from t.from to t.to
//   stage.caption(RX.describe(model, t));        fill the caption area (null clears it)
//   stage.speed = 2;                             2x faster
(function () {
  "use strict";

  const LOCK = '<svg class="sc-lock" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round"><rect x="5" y="11" width="14" height="10" rx="2"/><path d="M8 11V7a4 4 0 0 1 8 0v4"/></svg>';
  const WORKER_OPS = ["Spin", "WorkerExit", "WorkerExitBang", "FinishWorker", "ForkInWorker", "WorkerTrap"];

  const reduced = () => window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  const el = (tag, cls, text) => {
    const e = document.createElement(tag);
    if (cls) e.className = cls;
    if (text != null) e.textContent = text;
    return e;
  };

  function actorOf(model, t) {
    const a = t.args || [];
    switch (model.name) {
      case "RactorPorts": return t.op === "FinishWorker" ? { ractor: 2 } : { ractor: a[0] };
      case "RactorBlocking": return t.op === "Finish" || t.op === "FinishWorker" ? { ractor: 2 } : { thread: a[0] };
      case "RactorOrder": return t.op === "Send" || t.op === "Finish" ? { ractor: a[0] } : { ractor: 1 };
      case "RactorProcess":
        if (t.op === "Block" || t.op === "Kill") return { thread: a[0] };
        if (WORKER_OPS.includes(t.op)) return { thread: 2 };
        return { thread: 0 };
      case "RactorTransfer":
        if (t.op === "Receive") return { ractor: 2 };
        return { ractor: t.op === "Touch" ? a[0] : 1 };
    }
    if (t.op === "Finish" || t.op === "FinishWorker") return { ractor: 2 };
    const cast = RX.CAST[model.name] || { ractors: {} };
    return { ractor: cast.ractors[a[0]] ? a[0] : 1 };
  }

  function targetOf(model, t) {
    if (model.name === "RactorBlocking" && ["Raise", "Wakeup", "Kill"].includes(t.op)) return t.args[1];
    return null;
  }

  function sentPort(model, t) {
    if (t.op === "Send") return t.args[1];
    if (model.name === "RactorProcess" && t.op === "SendPort") return 1;
    return null;
  }

  function exceptionName(outcome) {
    const m = outcome.match(/^(.+):([a-z]+)$/);
    return m ? m[1] : outcome;
  }

  function shortLabel(label) {
    if (!label || label === "msg") return "";
    if (label.length <= 5) return label;
    const sym = label.match(/:(\w+)/);
    return sym ? sym[1].slice(0, 4) : label.slice(0, 4);
  }

  function envEl(m) {
    const e = el("div", "sc-env" + (m && m.from ? " " + (RX.COLORS[m.from] || "") : ""));
    const s = shortLabel(m && m.label);
    if (s) e.append(el("span", "sc-el", s));
    return e;
  }

  function mount(root, opts = {}) {
    root.classList.add("sc-stage");
    const rooms = el("div", "sc-rooms");
    const facts = el("div", "sc-facts");
    const caption = el("div", "sc-caption");
    const fx = el("div", "sc-fx");
    root.append(rooms, facts, caption, fx);

    const api = { speed: opts.speed || 1, scene: null, root };
    let currentJson = null;
    let bubble = null;

    const dur = (ms) => (reduced() ? 0 : ms / api.speed);
    const sleep = (ms) => new Promise((r) => setTimeout(r, dur(ms)));
    const finished = (anim) => anim.finished.catch(() => {});
    const q = (sel) => rooms.querySelector(sel);
    const threadEl = (id) => q(`[data-thread="${id}"]`);
    const portEl = (id) => q(`[data-port="${id}"]`);
    const nameEl = (id) => q(`[data-actor="${id}"]`);

    function findActor(scene, actor) {
      if (actor.thread != null) {
        const te = threadEl(actor.thread);
        if (te) return te;
        const th = scene.threads.find((x) => x.id === actor.thread);
        return nameEl(th ? th.ractor : 1) || nameEl(1);
      }
      return nameEl(actor.ractor) || nameEl(1);
    }

    function anchorOf(target) {
      return target.querySelector(".sc-dot") || target.querySelector(".sc-slot") || target;
    }

    function center(target) {
      const a = anchorOf(target);
      const r = a.getBoundingClientRect(), R = root.getBoundingClientRect();
      return { x: r.left + r.width / 2 - R.left, y: r.top + r.height / 2 - R.top, top: r.top - R.top };
    }

    function threadNode(th) {
      const e = el("div", `sc-thread ${th.status}`);
      e.dataset.thread = th.id;
      e.append(el("div", "sc-dot"), el("div", "sc-tl", th.label));
      if (th.status === "blocked") {
        e.append(el("span", "sc-zz", "z z"));
        if (th.waiting) e.append(el("div", "sc-wait", th.waiting));
      } else if (th.status === "running" && th.waiting) {
        const w = el("div", "sc-wait", th.waiting);
        w.style.color = "var(--o-runs)";
        e.append(w);
      }
      if (th.unwound) e.append(el("div", "sc-ens", "ensure ran"));
      return e;
    }

    function portNode(p) {
      const e = el("div", "sc-port" + (p.open ? "" : " closed"));
      e.dataset.port = p.id;
      const slot = el("div", "sc-slot");
      p.messages.slice(0, 2).forEach((m) => slot.append(envEl(m)));
      slot.insertAdjacentHTML("beforeend", LOCK);
      e.append(el("span", "sc-pl", p.label), slot);
      if (p.messages.length > 2) e.append(el("span", "sc-more", `+${p.messages.length - 2}`));
      if (p.note) e.append(el("span", "sc-note", p.note));
      return e;
    }

    function draw(scene) {
      rooms.replaceChildren();
      const ended = scene.facts.some((f) => f.label === "process" && f.value === "ended");
      scene.ractors.forEach((r, i) => {
        const room = el("section", `sc-room rx-ractor ${r.color}${r.alive ? "" : " dead"}`);
        room.dataset.ractor = r.id;
        const head = el("div", "sc-head");
        const name = el("span", "sc-name", r.name);
        name.dataset.actor = r.id;
        head.append(name, el("span", "sc-state", r.alive ? "" : ended ? "process ended" : "finished"));
        const body = el("div", "sc-body");
        const threads = scene.threads.filter((th) => th.ractor === r.id);
        if (threads.length) threads.forEach((th) => body.append(threadNode(th)));
        else body.append(el("span", "sc-empty", r.alive ? "its block is running" : "its block has ended"));
        const side = i === 0 ? "right" : "left";
        const ports = el("div", `sc-ports ${side}`);
        const own = scene.ports.filter((p) => p.owner === r.id);
        own.forEach((p) => ports.append(portNode(p)));
        if (own.length) room.classList.add(`has-${side}`);
        room.append(head, body, ports);
        rooms.append(room);
      });
      facts.replaceChildren(...scene.facts.map((f) => {
        const s = el("span", null, `${f.label}: `);
        s.dataset.fact = f.label;
        s.append(el("b", null, f.value));
        return s;
      }));
      api.scene = scene;
      currentJson = JSON.stringify(scene);
    }

    function clearFx() {
      fx.replaceChildren();
      bubble = null;
      rooms.querySelectorAll(".sc-tag, .sc-spark, .sc-childl").forEach((x) => x.remove());
    }

    function showBubble(target, text) {
      if (!text) return;
      bubble = el("div", "sc-bubble", text);
      fx.append(bubble);
      const c = center(target);
      bubble.style.left = `${c.x}px`;
      bubble.style.top = `${c.top - 8}px`;
      const r = bubble.getBoundingClientRect(), R = root.getBoundingClientRect();
      const over = r.right - R.right + 8;
      if (over > 0) bubble.style.left = `${c.x - over}px`;
      if (r.left < R.left) bubble.style.left = `${c.x + (R.left - r.left) + 8}px`;
    }

    function tag(target, kind, text) {
      if (!target || !text) return;
      target.append(el("span", `sc-tag ${kind}`, text));
    }

    function flash(target, cls, ms) {
      if (!target) return Promise.resolve();
      target.classList.add(cls);
      return sleep(ms).then(() => target.classList.remove(cls));
    }

    function fly(fromEl, toEl, proto) {
      if (!fromEl || !toEl) return Promise.resolve();
      const env = proto ? proto.cloneNode(true) : envEl({ label: "msg" });
      fx.append(env);
      const s = center(fromEl), e = center(toEl);
      const at = (p, sc) => `translate(${p.x - 12}px, ${p.y - 8}px) scale(${sc})`;
      const mid = { x: (s.x + e.x) / 2, y: Math.min(s.y, e.y) - 36 };
      const anim = env.animate(
        [{ transform: at(s, 0.5), opacity: 0.3 }, { transform: at(mid, 1.15), opacity: 1, offset: 0.55 }, { transform: at(e, 1), opacity: 1 }],
        { duration: dur(700), easing: "ease-in-out", fill: "forwards" }
      );
      return finished(anim).then(() => env.remove());
    }

    function exitCard(t, A, B) {
      const card = el("div", "sc-card");
      card.append(el("div", "sc-ch", `Process ended with exit status ${t.exit.status}`));
      const ul = el("ul");
      ul.append(el("li", t.exit.atExit ? "yes" : "no", t.exit.atExit ? "The at_exit handler ran." : "No at_exit handler ran."));
      const now = B.threads.filter((th) => th.unwound && !(A.threads.find((x) => x.id === th.id) || {}).unwound).map((th) => th.label);
      ul.append(el("li", now.length ? "ens" : "no", now.length ? `${now.join(" and ")} unwound: ${now.length > 1 ? "their" : "its"} ensure clauses ran.` : "No thread unwound."));
      if (t.exit.stderr) {
        const li = el("li", null, "stderr: ");
        li.append(el("span", "sc-cc", t.exit.stderr));
        ul.append(li);
      }
      card.append(ul);
      fx.append(card);
    }

    async function forkSplit() {
      const ghost = rooms.cloneNode(true);
      ghost.classList.add("sc-ghost");
      const r = rooms.getBoundingClientRect(), R = root.getBoundingClientRect();
      Object.assign(ghost.style, { left: `${r.left - R.left}px`, top: `${r.top - R.top}px`, width: `${r.width}px`, height: `${r.height}px` });
      ghost.append(el("span", "sc-ghostl", "parent process keeps running"));
      fx.append(ghost);
      const g = ghost.animate(
        [{ transform: "translate(0,0) scale(1)", opacity: 0.95 }, { transform: "translate(-60px,-40px) scale(.9)", opacity: 0 }],
        { duration: dur(1000), easing: "ease-in", fill: "forwards" }
      );
      const c = rooms.animate(
        [{ transform: "translate(0,0)", opacity: 1 }, { transform: "translate(28px, 14px)", opacity: 0.5, offset: 0.35 }, { transform: "translate(0,0)", opacity: 1 }],
        { duration: dur(1000), easing: "ease-in-out" }
      );
      await Promise.all([finished(g), finished(c)]);
      ghost.remove();
    }

    function wakeOk(t) {
      return (t.wakes || []).find((w) => RX.kind(w.outcome) === "ok");
    }

    async function play(model, t) {
      const A = RX.scene(model, t.from), B = RX.scene(model, t.to);
      const d = RX.describe(model, t);
      if (JSON.stringify(A) !== currentJson) draw(A);
      clearFx();
      const actor = actorOf(model, t);
      const k = RX.kind(t.outcome);
      const actorEl = findActor(A, actor);

      actorEl.classList.add("sc-acting");
      showBubble(actorEl, d.ruby);
      await sleep(550);

      const jobs = [];
      let flewToWaiter = false;
      const woken = wakeOk(t);
      for (const pB of B.ports) {
        const pA = A.ports.find((p) => p.id === pB.id);
        if (!pA) continue;
        const pe = portEl(pB.id);
        if (pB.messages.length > pA.messages.length) {
          const added = pB.messages.slice(pA.messages.length);
          jobs.push(added.reduce((p, m, i) => p.then(() => (i ? sleep(120) : null)).then(() => fly(actorEl, pe, envEl(m))), Promise.resolve()));
        } else if (pB.messages.length < pA.messages.length) {
          const oldest = pe.querySelector(".sc-env");
          if (oldest) oldest.style.visibility = "hidden";
          const receiver = woken ? threadEl(woken.thread) : actorEl;
          flewToWaiter = !!woken;
          jobs.push(fly(pe, receiver || actorEl, oldest));
        }
        if (pA.open && !pB.open) jobs.push(flash(pe, "sc-closing", 500).then(() => pe.classList.add("closed")));
      }
      if (woken && !flewToWaiter) {
        const via = sentPort(model, t);
        const pe = via != null ? portEl(via) : null;
        const dest = threadEl(woken.thread) || findActor(A, { thread: woken.thread });
        jobs.push(pe ? fly(actorEl, pe).then(() => fly(pe, dest)) : fly(actorEl, dest));
      }
      for (const r of B.ractors) {
        const rA = A.ractors.find((x) => x.id === r.id);
        if (rA && rA.alive && !r.alive && t.op !== "Fork" && k !== "exit") jobs.push(flash(q(`[data-ractor="${r.id}"]`), "sc-fade", 600));
      }
      const target = targetOf(model, t);
      if (target != null && !(t.wakes || []).some((w) => w.thread === target)) jobs.push(flash(threadEl(target), "sc-nudge", 500));
      await Promise.all(jobs);
      if (jobs.length) await sleep(120);

      if (t.op === "Fork" && t.outcome === "ok") await forkSplit();

      if (bubble) bubble.remove();
      bubble = null;
      draw(B);
      if (t.op === "Fork" && t.outcome === "ok") rooms.append(el("span", "sc-childl", "child process"));

      const after = [];
      const actorB = findActor(B, actor);
      const resultText = (d.sentences[0] || "").replace(/^The call /, "").replace(/\.$/, "");
      for (const f of B.facts) {
        const before = A.facts.find((x) => x.label === f.label);
        if (before && before.value !== f.value) {
          const fe = [...facts.children].find((x) => x.dataset.fact === f.label);
          if (fe) after.push(flash(fe, "sc-changed", 1200));
        }
      }
      if (k === "err") { after.push(flash(actorB, "sc-shake", 500)); tag(actorB, "err", exceptionName(t.outcome)); }
      else if (k === "nil") tag(actorB, "nil", "returns nil");
      else if (k === "blocks") after.push(flash(actorB, "sc-settle", 600));
      else if (k === "runs") tag(actorB, "runs", "keeps running");
      else if (k === "ok" && t.value) { after.push(flash(actorB, "sc-pulse", 700)); tag(actorB, "ok", resultText); }
      else if (k === "exit") exitCard(t, A, B);

      for (const w of t.wakes || []) {
        const te = threadEl(w.thread);
        if (!te) continue;
        const wk = RX.kind(w.outcome);
        const sentence = (d.sentences.find((s) => s.startsWith(RX.threadName(model, w.thread) + " wakes")) || "").split("its call ")[1] || "";
        if (wk === "ok") { after.push(flash(te, "sc-pulse", 700)); tag(te, "ok", sentence.replace(/\.$/, "") || "wakes"); }
        else if (wk === "killed") { after.push(flash(te, "sc-killed", 700)); te.append(el("span", "sc-spark", "ensure runs")); tag(te, "killed", "killed"); }
        else if (wk === "err") { after.push(flash(te, "sc-shake", 500)); tag(te, "err", exceptionName(w.outcome)); }
        else tag(te, wk, sentence.replace(/\.$/, ""));
      }
      await Promise.all(after);
      await sleep(250);
    }

    function render(scene) {
      clearFx();
      draw(scene);
    }

    function setCaption(desc) {
      caption.replaceChildren();
      if (!desc) return;
      if (typeof desc === "string") { caption.append(el("div", "sc-cq", desc)); return; }
      caption.append(el("div", "sc-ct", desc.title));
      const p = el("p", "sc-cs");
      desc.sentences.forEach((s, i) => {
        const span = el("span", desc.kind === "err" && i === 0 ? "err" : null, s + " ");
        p.append(span);
      });
      caption.append(p);
      if (desc.ruby) caption.append(el("pre", "sc-code", desc.ruby));
    }

    api.render = render;
    api.play = play;
    api.caption = setCaption;
    api.actorOf = (model, t) => actorOf(model, t);
    api.dur = dur;
    return api;
  }

  window.RXScene = { mount, actorOf, exceptionName };
})();
