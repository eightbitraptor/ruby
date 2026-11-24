// Shared helpers for the visualisation prototypes.
//
// Load the data files first, then this file:
//   <script src="data/RactorBlocking.js"></script>
//   <script src="common.js"></script>
//
// RX.load(name)            model with indexes (byId, out, inn, initial)
// RX.scene(model, state)   cast and state of one model state, for drawing
// RX.describe(model, t)    plain-English title, sentences, and Ruby of a transition
// RX.kind(outcome)         ok | nil | blocks | runs | err | killed | exit
// RX.stories(model)        curated scenarios: [{id, title, why, path: [transitions]}]
// RX.shortest(model, test) shortest path from an initial state to a transition that passes test
// RX.rowKey(model, t)      the outcome row of t: "op|aspect|outcome", as the site's Calls tables group them
// RX.scope(model, paths)   {shown, total}: outcome rows that the paths visit, out of all rows
// RX.diff(model, t)        variables that t changes: [{name, hint, before, after}] in TLA+ notation
// RX.fmt(value, path, model)  one value in TLA+ notation
(function () {
  "use strict";

  const COLORS = { 1: "main", 2: "worker", 3: "worker2" };
  const ERROR_RULES = {
    creator: "only the Ractor that created a port may receive from it",
    closer: "only the Ractor that created a port may close it",
    taken: "the value was already taken",
    successor: "another Ractor took the value",
  };

  function load(name) {
    const data = (window.RACTOR_MODELS || {})[name];
    if (!data) throw new Error(`model ${name} is not loaded; add <script src="data/${name}.js">`);
    if (data._indexed) return data;
    data.byId = data.cores;
    data.out = data.cores.map(() => []);
    data.inn = data.cores.map(() => []);
    data.transitions.forEach((t, i) => {
      t.id = i;
      data.out[t.from].push(t);
      data.inn[t.to].push(t);
    });
    data.initial = data.cores.filter((c) => c.initial).map((c) => c.id);
    data.aspects = [...new Set(data.cores.map((c) => c.fields.aspect))].sort();
    data._indexed = true;
    return data;
  }

  function kind(outcome) {
    if (["ok", "true", "false"].includes(outcome)) return "ok";
    if (["nil", "blocks", "runs", "killed", "exit"].includes(outcome)) return outcome;
    return "err";
  }

  // ------------------------------------------------------------------ cast

  const CAST = {
    RactorPorts: {
      ractors: { 1: "main", 2: "worker" },
      ports: { 1: { label: "D", owner: 2, note: "worker's default port" }, 2: { label: "P1", owner: 1 }, 3: { label: "P2", owner: 1 } },
      threads: {},
    },
    RactorBlocking: {
      ractors: { 1: "main", 2: "worker" },
      ports: { 1: { label: "D", owner: 2, note: "worker's default port" }, 2: { label: "P1", owner: 1 }, 3: { label: "P2", owner: 1 }, M: { label: "M", owner: 1, note: "monitors the worker" } },
      threads: { 1: { label: "T1", ractor: 1 }, 2: { label: "T2", ractor: 1 }, 3: { label: "T3", ractor: 2 }, 4: { label: "T4", ractor: 2 } },
      active: { main: [1, 2, 3], worker: [1, 3, 4], exit: [1, 2], abort: [1, 2] },
      usedPorts: { main: [2, 3], worker: [1], exit: ["M"], abort: ["M"] },
    },
    RactorOrder: {
      ractors: { 1: "main", 2: "W1", 3: "W2" },
      ports: { 1: { label: "P1", owner: 1 }, 2: { label: "P2", owner: 1 } },
      threads: {},
    },
    RactorProcess: {
      ractors: { 1: "main", 2: "worker" },
      ports: { 1: { label: "P1", owner: 1 } },
      threads: { 0: { label: "main thread", ractor: 1 }, 1: { label: "T1", ractor: 1 }, 2: { label: "T2", ractor: 2 } },
    },
    RactorTermination: {
      subject: "target",
      ractors: { 1: "main", 2: "target", 3: "observer" },
      ports: { 1: { label: "M", owner: 1, note: "main's monitor port" }, 2: { label: "N", owner: 3, note: "observer's monitor port" } },
      threads: {},
    },
    RactorTransfer: {
      ractors: { 1: "main", 2: "worker" },
      ports: { 1: { label: "D", owner: 2, note: "worker's default port" } },
      threads: {},
    },
    RactorIsolation: { ractors: { 1: "main", 2: "worker" }, ports: {}, threads: {} },
    RactorLocal: { ractors: { 1: "main", 2: "worker" }, ports: {}, threads: {} },
  };

  const TRANSFER_NAMES = { string: ["string", "-"], pair: ["array", "inner string"], cycle: ["cyclic array", "inner string"],
    frozen: ["frozen array", "inner string"], thread: ["Thread", "-"] };
  const MAIN_STATE = { mutable: "mutable", frozen: "frozen", moved: "moved shell (Ractor::MovedObject)" };
  const HELD = { shared: "the same object", mutable: "a distinct object", frozen: "a distinct frozen object" };
  const CAPTURE = { 1: "captures nothing", 2: "reads a shareable outer variable", 3: "reads an unshareable outer variable",
    4: "reads a reassigned outer variable", 5: "has a shareable self", 6: "has an unshareable self" };
  const PROC_RUBY = { 1: "Ractor.shareable_proc { 1 }", 2: "s = \"shared\".freeze; Ractor.shareable_proc { s }",
    3: "a = []; Ractor.shareable_proc { a }", 4: "x = 1; x = 2; Ractor.shareable_proc { x }",
    5: "Ractor.shareable_proc(self: \"shared\".freeze) { self }", 6: "Ractor.shareable_proc(self: +\"self\") { self }" };
  const BLOCK_RUBY = { 1: "Ractor.new { 1 }", 2: "s = \"shared\".freeze; Ractor.new { s }", 3: "a = []; Ractor.new { a }",
    4: "x = 1; x = 2; Ractor.new { x }" };
  const SLOT = {
    const: ["constant SLOT", "const_set(:SLOT, %v)", "const_get(:SLOT)"],
    readonly: ["constant SLOT", "const_set(:SLOT, %v)", "const_get(:SLOT)"],
    ivar: ["instance variable @slot", "instance_variable_set(:@slot, %v)", "instance_variable_get(:@slot)"],
    cvar: ["class variable @@slot", "class_variable_set(:@@slot, %v)", "class_variable_get(:@@slot)"],
  };

  function threadName(model, tid) {
    const t = CAST[model.name].threads[tid];
    return t ? `${t.label} (${CAST[model.name].ractors[t.ractor]})` : `thread ${tid}`;
  }
  function ractorName(model, rid) { return CAST[model.name].ractors[rid] || `Ractor ${rid}`; }
  function portName(model, pid) { return (CAST[model.name].ports[pid] || {}).label || `port ${pid}`; }

  // ----------------------------------------------------------------- scene

  // A scene is what a picture of one state needs:
  //   ractors: [{id, name, color, alive}]
  //   threads: [{id, ractor, label, status, waiting, unwound}]   status: idle|blocked|running|dead
  //   ports:   [{id, owner, label, note, open, messages: [{label, from}]}]
  //   facts:   [{label, value}]
  function scene(model, state) {
    const f = (typeof state === "number" ? model.byId[state] : state).fields;
    const cast = CAST[model.name];
    const ractor = (id, alive) => ({ id: Number(id), name: cast.ractors[id], color: COLORS[id], alive });
    const out = { aspect: f.aspect, ractors: [], threads: [], ports: [], facts: [] };
    if (model.name === "RactorPorts") {
      out.ractors = [ractor(1, true), ractor(2, f.workerAlive)];
      out.ports = [1, 2, 3].map((p) => ({ id: p, ...cast.ports[p], open: f.portOpen[p - 1], messages: Array(f.queue[p - 1]).fill({ label: "msg" }) }));
    } else if (model.name === "RactorBlocking") {
      out.ractors = [ractor(1, true), ractor(2, f.alive)];
      out.threads = cast.active[f.aspect].map((tid) => ({
        id: tid, ractor: cast.threads[tid].ractor, label: cast.threads[tid].label, status: f.ts[tid - 1],
        waiting: f.wait[tid - 1].op === "none" ? null : waitText(model, f.wait[tid - 1]), unwound: false,
      }));
      out.ports = cast.usedPorts[f.aspect].map((p) => p === "M"
        ? { id: "M", ...cast.ports.M, open: true, messages: f.token ? [{ label: f.aspect === "exit" ? "[worker, :exited]" : "[worker, :aborted]" }] : [] }
        : { id: p, ...cast.ports[p], open: f.portOpen[p - 1], messages: Array(f.queue[p - 1]).fill({ label: "msg" }) });
      if (f.aspect === "exit" || f.aspect === "abort") out.facts.push({ label: "worker's value taken", value: f.taken ? "yes" : "no" });
    } else if (model.name === "RactorOrder") {
      out.ractors = [1, 2, 3].map((r) => ractor(r, f.alive[r - 1]));
      out.ports = [1, 2].map((p) => ({ id: p, ...cast.ports[p], open: f.portOpen[p - 1], messages: f.queue[p - 1].map(([s, n]) => ({ label: `m${s}.${n}`, from: s })) }));
      out.facts.push({ label: "messages sent", value: `main ${f.sent[0]}, W1 ${f.sent[1]}, W2 ${f.sent[2]}` });
      if (f.aspect === "mixed") out.facts.push({ label: "value taken", value: `W1 ${f.taken[1] ? "yes" : "no"}, W2 ${f.taken[2] ? "yes" : "no"}` });
    } else if (model.name === "RactorProcess") {
      out.ractors = [ractor(1, !f.ended), ractor(2, f.alive && !f.ended)];
      out.threads = [
        { id: 0, ractor: 1, label: "main thread", status: f.ended ? "dead" : "idle", waiting: null, unwound: false },
        ...[1, 2].map((tid) => ({ id: tid, ractor: tid === 1 ? 1 : 2, label: `T${tid}`, status: f.ts[tid - 1],
          waiting: f.ts[tid - 1] === "blocked" ? (tid === 1 ? "P1.receive" : "Ractor.receive") : f.ts[tid - 1] === "running" ? "loop { Thread.pass }" : null,
          unwound: f.unwound[tid - 1] })),
      ];
      out.ports = [{ id: 1, ...cast.ports[1], open: true, messages: Array(f.queued).fill({ label: "msg" }) }];
      out.facts.push({ label: "process", value: f.ended ? "ended" : f.side === "child" ? "forked child" : "parent" });
      if (f.atExit) out.facts.push({ label: "at_exit handler", value: "registered" });
      if (f.aspect === "signal") out.facts.push({ label: "USR1 handler runs", value: String(f.handled) });
      if (f.result !== "none") out.facts.push({ label: "worker's value", value: { ready: "ready", nil: "nil (child of a fork)", taken: "taken" }[f.result] });
    } else if (model.name === "RactorTermination") {
      const token = f.aspect === "exit" ? "[target, :exited]" : "[target, :aborted]";
      out.ractors = [ractor(1, true), ractor(2, f.alive), ractor(3, true)];
      out.ports = [1, 2].map((p) => ({ id: p, ...cast.ports[p], open: true, messages: Array(f.tokens[p - 1]).fill({ label: token }) }));
      out.facts.push({ label: "target", value: f.alive ? "running" : f.aspect === "exit" ? "returned :finished" : "raised an exception" });
      out.facts.push({ label: "monitor registrations", value: `M ${f.monitors[0]}, N ${f.monitors[1]}` });
      out.facts.push({ label: "value taken by", value: f.successor ? cast.ractors[f.successor] : "nobody" });
    } else if (model.name === "RactorTransfer") {
      const name = TRANSFER_NAMES[f.aspect];
      out.ractors = [ractor(1, true), ractor(2, true)];
      out.ports = [{ id: 1, ...cast.ports[1], open: true,
        messages: [0, 1].filter((i) => f.inbox[i] !== "none").map((i) => ({ label: `${name[i]}: ${HELD[f.inbox[i]]}`, from: 1 })) }];
      [0, 1].forEach((i) => {
        if (f.mainState[i] !== "absent") {
          out.facts.push({ label: `main's ${name[i]}`, value: MAIN_STATE[f.mainState[i]] + (f.shareable[i] && f.mainState[i] !== "moved" ? ", shareable" : "") });
        }
      });
      [0, 1].forEach((i) => { if (f.workerHas[i] !== "none") out.facts.push({ label: `worker's ${name[i]}`, value: HELD[f.workerHas[i]] }); });
    } else if (model.name === "RactorIsolation") {
      out.ractors = [ractor(1, true), ractor(2, f.alive)];
      const slot = SLOT[f.aspect];
      ["K", "L"].forEach((k, i) => {
        const parts = [];
        if (slot) parts.push(`${slot[0]}: ${f.slot[i]}`);
        if (f.aspect === "method") parts.push(f.defined[i] ? "slot_method defined" : "no slot_method");
        if (f.aspect === "include") parts.push(f.included[i] ? "module included" : "module not included");
        if (f.aspect === "freeze" || f.frozenClass[i]) parts.push(f.frozenClass[i] ? "frozen" : "not frozen");
        out.facts.push({ label: `class ${k} (${i === 0 ? "main's" : "worker's"})`, value: parts.join(", ") || "unchanged" });
      });
    } else if (model.name === "RactorLocal") {
      out.ractors = [ractor(1, true), ractor(2, true)];
      ["main", "worker"].forEach((r, i) => out.facts.push({ label: `${r}'s store`, value: `:k1 ${f.stored[i][0]}, :k2 ${f.stored[i][1]}` }));
    }
    return out;
  }

  function waitText(model, wait) {
    const p = wait.ports.map((x) => portName(model, x));
    switch (wait.op) {
      case "Receive": return p[0] === "D" ? "Ractor.receive" : `${p[0]}.receive`;
      case "ReceiveTimed": return p[0] === "D" ? "Ractor.receive(timeout: 600)" : `${p[0]}.receive(timeout: 600)`;
      case "Select": return `Ractor.select(${p.join(", ")})`;
      case "Join": return "worker.join";
      case "Value": return "worker.value";
      case "SelectRactor": return "Ractor.select(worker)";
      case "ReceiveToken": return "M.receive";
      default: return wait.op;
    }
  }

  // -------------------------------------------------------------- narration

  function valueText(model, op, value) {
    if (!value) return null;
    const subject = CAST[model.name].subject || "worker";
    const fixed = { msg: "the message", self: `the ${subject}`, finished: ":finished", selected: `[${subject}, :finished]`,
      exited: `[${subject}, :exited]`, aborted: `[${subject}, :aborted]`, child: "nil (this is the child)", fresh: ":fresh" };
    if (fixed[value]) return fixed[value];
    if (/^P\d$|^D$/.test(value)) return `[${value}, the message]`;
    if (/^P\d m\d\.\d$/.test(value)) { const [p, m] = value.split(" "); return `[${p}, ${m}]`; }
    if (/^m\d\.\d$/.test(value)) return `message ${value}`;
    if (/^W\d$/.test(value)) return `[${value}, :finished]`;
    if (op === "WorkerExit") return `a worker whose value raises Ractor::RemoteError caused by ${value}`;
    return value;
  }

  function resultText(model, op, outcome, value) {
    const k = kind(outcome);
    if (outcome === "true" || outcome === "false") return `returns ${outcome}`;
    if (k === "ok") { const v = valueText(model, op, value); return v ? `returns ${v}` : "succeeds"; }
    if (k === "nil") return "returns nil";
    if (k === "blocks") return "waits";
    if (k === "runs") return "keeps running";
    if (k === "killed") return "is killed inside the call; its ensure clauses run";
    if (k === "exit") return "ends the process";
    const tagged = outcome.match(/^(.+):([a-z]+)$/);
    const [cls, rule] = tagged ? [tagged[1], tagged[2]] : [outcome, null];
    return rule ? `raises ${cls} (${ERROR_RULES[rule] || rule})` : `raises ${cls}`;
  }

  function wakesText(model, t) {
    return (t.wakes || []).map((w) => `${threadName(model, w.thread)} wakes: its call ${resultText(model, null, w.outcome, w.value)}.`);
  }

  const say = {
    RactorPorts(model, t) {
      const [c, p] = t.args;
      const who = ractorName(model, c);
      const port = portName(model, p);
      const ref = (x) => (x === 1 ? (c === 2 ? "Ractor.current.default_port" : "worker.default_port") : portName(model, x).toLowerCase());
      const sel = (a) => `Ractor.select(${a.slice(1).map((x) => portName(model, x)).join(", ")})`;
      const selRuby = (a, extra) => `Ractor.select(${a.slice(1).map(ref).join(", ")}${extra})`;
      switch (t.op) {
        case "Send": return [`${who} sends a message to ${port}`, p === 1 ? (c === 2 ? "Ractor.current.send(:msg)" : "worker.send(:msg)") : `${ref(p)}.send(:msg)`];
        case "Receive": return [`${who} receives on ${port}`, p === 1 && c === 2 ? "Ractor.receive" : `${ref(p)}.receive`];
        case "Poll": return [`${who} polls ${port}`, p === 1 && c === 2 ? "Ractor.receive(timeout: 0)" : `${ref(p)}.receive(timeout: 0)`];
        case "Close": return [`${who} closes ${port}`, p === 1 ? (c === 2 ? "Ractor.current.close" : "worker.close") : `${ref(p)}.close`];
        case "Select": return [`${who} calls ${sel(t.args)}`, selRuby(t.args, "")];
        case "SelectPoll": return [`${who} polls ${sel(t.args)}`, selRuby(t.args, ", timeout: 0")];
        case "FinishWorker": return ["The worker's block returns; its ports close", "# the worker's block ends"];
      }
    },
    RactorBlocking(model, t) {
      const [tid, p] = t.args;
      const who = threadName(model, tid);
      const port = p ? portName(model, p) : null;
      const inWorker = tid >= 3;
      const target = t.args[1] ? `T${t.args[1]}` : null;
      switch (t.op) {
        case "Send": return [`${who} sends a message to ${port}`, port === "D" ? (inWorker ? "Ractor.current.send(:msg)" : "worker.send(:msg)") : `${port.toLowerCase()}.send(:msg)`];
        case "Receive": return [`${who} receives on ${port}`, port === "D" ? "Ractor.receive" : `${port.toLowerCase()}.receive`];
        case "ReceiveTimed": return [`${who} receives on ${port} with a 600 s timeout`, port === "D" ? "Ractor.receive(timeout: 600)" : `${port.toLowerCase()}.receive(timeout: 600)`];
        case "ReceiveShort": return [`${who} receives on ${port} with a 0.05 s timeout`, port === "D" ? "Ractor.receive(timeout: 0.05)" : `${port.toLowerCase()}.receive(timeout: 0.05)`];
        case "Select": return [`${who} waits on P1 and P2 at once`, "Ractor.select(p1, p2)"];
        case "Close": return [`${who} closes ${port}`, port === "D" ? "Ractor.current.close" : `${port.toLowerCase()}.close`];
        case "Raise": return [`${who} raises an exception in ${target}`, `${target.toLowerCase()}.raise(RuntimeError)`];
        case "Wakeup": return [`${who} wakes ${target} up`, `${target.toLowerCase()}.wakeup`];
        case "Kill": return [`${who} kills ${target}`, `${target.toLowerCase()}.kill`];
        case "Join": return [`${who} joins the worker`, "worker.join"];
        case "Value": return [`${who} asks for the worker's value`, "worker.value"];
        case "SelectRactor": return [`${who} selects on the worker`, "Ractor.select(worker)"];
        case "ReceiveToken": return [`${who} receives on the monitor port M`, "m.receive  # m = port given to worker.monitor"];
        case "Finish": return [model.aspect === "abort" || t.wakes.some((w) => w.outcome === "Ractor::RemoteError") ? "The worker's block raises" : "The worker's block returns :finished", "# the worker's block ends"];
        case "FinishWorker": return ["The worker's block returns", "# the worker's block ends"];
      }
    },
    RactorOrder(model, t, from) {
      const sender = (s) => ractorName(model, s);
      const src = (a) => (a[0] === "port" ? portName(model, a[1]) : ractorName(model, a[1]));
      switch (t.op) {
        case "Send": {
          const [s, p] = t.args;
          const n = from ? from.fields.sent[s - 1] + 1 : "n";
          return [`${sender(s)} sends message m${s}.${n} to ${portName(model, p)}`, `${portName(model, p).toLowerCase()}.send([${s}, ${n}])`];
        }
        case "Poll": return [`main takes the oldest message from ${portName(model, t.args[0])}`, `${portName(model, t.args[0]).toLowerCase()}.receive(timeout: 0)`];
        case "Close": return [`main closes ${portName(model, t.args[0])}`, `${portName(model, t.args[0]).toLowerCase()}.close`];
        case "Select": return [`main selects on ${t.args.map(src).join(" and ")}`, `Ractor.select(${t.args.map((a) => src(a).toLowerCase()).join(", ")}, timeout: 0)`];
        case "Finish": return [`${sender(t.args[0])}'s block returns :finished`, "# the worker's block ends"];
        case "Value": return [`main asks for ${sender(t.args[0])}'s value`, `${sender(t.args[0]).toLowerCase()}.value`];
      }
    },
    RactorProcess(model, t) {
      const map = {
        Block: t.args[0] === 1 ? ["T1 (main) receives on P1", "p1.receive"] : ["T2 (worker) receives on its default port", "Ractor.receive"],
        Spin: ["T2 (worker) runs a loop that never sleeps", "loop { Thread.pass }"],
        SendPort: ["main's main thread sends a message to P1", "p1.send(:msg)"],
        Poll: ["main's main thread polls P1", "p1.receive(timeout: 0)"],
        AtExit: ["main registers an at_exit handler", "at_exit { puts \"at_exit\" }"],
        WorkerExit: ["the worker's main thread calls exit", "exit  # inside the worker"],
        FinishWorker: ["the worker's block returns :finished", "# the worker's block ends"],
        MainReturn: ["main reaches the end of the script", "# end of the main script"],
        MainExit: ["main calls exit 3", "exit 3"],
        MainRaise: ["main raises an exception nobody rescues", "raise \"main boom\""],
        MainExitBang: ["main calls exit! 4", "exit! 4"],
        WorkerExitBang: ["the worker calls exit! 5", "exit! 5  # inside the worker"],
        Fork: ["main forks the process", "fork"],
        Count: ["main counts the Ractors", "Ractor.count"],
        SendWorker: ["main sends to the worker", "worker.send(:msg)"],
        Join: ["main joins the worker", "worker.join"],
        Value: ["main asks for the worker's value", "worker.value"],
        NewRactor: ["main starts a new Ractor and takes its value", "Ractor.new { :fresh }.value"],
        ForkInWorker: ["T2 (worker) tries to fork", "fork { exit! 0 }"],
        Kill: [`${t.args[0] === 1 ? "T1 (main)" : "T2 (worker)"} sends SIGUSR1 to the process`, "Process.kill(:USR1, Process.pid)"],
        Interrupted: ["main's main thread waits on P1 and receives SIGINT", "p1.receive  # Ctrl-C arrives"],
        WorkerTrap: [t.args[1] === 1 ? "T2 (worker) installs a signal handler" : "T2 (worker) installs a handler that captures a string", t.args[1] === 1 ? "Signal.trap(:USR2) { nil }" : "s = +\"x\"; Signal.trap(:USR2) { s }"],
      };
      return map[t.op];
    },
    RactorTermination(model, t) {
      const [c, p] = t.args;
      const who = ractorName(model, c);
      const port = p ? portName(model, p) : "";
      const pr = port.toLowerCase();
      switch (t.op) {
        case "Monitor": return [`${who} registers ${port} as a monitor of the target`, `target.monitor(${pr})`];
        case "Unmonitor": return [`${who} unregisters ${port}`, `target.unmonitor(${pr})`];
        case "Finish": return [model.aspect === "abort" ? "The target's block raises an exception" : "The target's block returns :finished", "# the target's block ends"];
        case "ReceiveToken": return [`${who} polls ${port} for a termination token`, `${pr}.receive(timeout: 0)`];
        case "Join": return [`${who} joins the target`, "target.join"];
        case "Value": return [`${who} asks for the target's value`, "target.value"];
        case "SelectRactor": return [`${who} selects on the target`, "Ractor.select(target, timeout: 0)"];
      }
    },
    RactorTransfer(model, t, from) {
      const names = TRANSFER_NAMES[from.fields.aspect];
      const o = t.op === "Touch" ? t.args[1] : t.args[0];
      const obj = `the ${names[o - 1]}`;
      const v = o === 1 ? "obj" : "inner";
      switch (t.op) {
        case "Freeze": return [`main freezes ${obj}`, `${v}.freeze`];
        case "MakeShareable": return [`main makes ${obj} shareable`, `Ractor.make_shareable(${v})`];
        case "MakeShareableCopy": return [`main makes a shareable copy of ${obj}`, `Ractor.make_shareable(${v}, copy: true)`];
        case "SendCopy": return [`main sends ${obj} to the worker`, `worker.send(${v})`];
        case "SendMove": return [`main moves ${obj} to the worker`, `worker.send(${v}, move: true)`];
        case "Receive": return ["The worker receives", "Ractor.receive(timeout: 0)"];
        case "Touch": return [`${ractorName(model, t.args[0])} calls a method on ${t.args[0] === 1 ? "its" : "the received"} ${names[o - 1]}`, `${v}.class`];
        case "NewRactorArg": return [`main starts a Ractor with ${obj} as its argument`, `Ractor.new(${v}) { |x| x.class }`];
      }
    },
    RactorIsolation(model, t, from) {
      const [c, x, k] = t.args;
      const who = ractorName(model, c);
      const cls = (n) => (n === 1 ? "K" : n === 2 ? "L" : "an object of its own");
      const slot = SLOT[from.fields.aspect] || ["slot", "", ""];
      const val = k === 1 ? "1" : "[1]";
      switch (t.op) {
        case "Set": return [`${who} sets ${slot[0]} of ${cls(x)} to ${k === 1 ? "a shareable" : "an unshareable"} value`, `${cls(x)}.${slot[1].replace("%v", val)}`];
        case "Get": return [`${who} reads ${slot[0]} of ${cls(x)}`, `${cls(x)}.${slot[2]}`];
        case "DefineMethod": return [`${who} defines a method in ${cls(x)}`, `${cls(x)}.class_eval("def slot_method; end")`];
        case "RemoveMethod": return [`${who} removes the method from ${cls(x)}`, `${cls(x)}.send(:remove_method, :slot_method)`];
        case "AliasMethod": return [`${who} aliases the method in ${cls(x)}`, `${cls(x)}.alias_method(:slot_alias, :slot_method)`];
        case "Include": return [`${who} includes its own module into ${cls(x)}`, `${cls(x)}.include(mod)`];
        case "FreezeClass": return [`${who} freezes ${cls(x)}`, `${cls(x)}.freeze`];
        case "GlobalGet": return [`${who} reads the global $gv`, "$gv"];
        case "GlobalSet": return [`${who} assigns the global $gv`, "$gv = 2"];
        case "StderrGet": return [`${who} reads $stderr`, "$stderr"];
        case "CallUnsafe": return [`${who} calls a C method that is not marked Ractor-safe`, "ObjectSpace.memsize_of(\"x\")"];
        case "AtExit": return [`${who} registers an at_exit handler`, "at_exit {}"];
        case "RegexpTimeout": return [`${who} sets Regexp.timeout`, "Regexp.timeout = nil"];
        case "DefineFinalizer": return [`${who} defines a finalizer on ${cls(x)}`, `ObjectSpace.define_finalizer(${x === 3 ? "Object.new" : cls(x)}, proc {})`];
        case "ShareableProc": return [`${who} makes a shareable proc that ${CAPTURE[x]}`, PROC_RUBY[x]];
        case "NewRactor": return [`${who} starts a Ractor whose block ${CAPTURE[x]}`, BLOCK_RUBY[x]];
        case "DupClass": return [`${who} duplicates ${cls(x)}`, `${cls(x)}.dup`];
        case "Instantiate": return [`${who} creates an instance of ${cls(x)}`, `${cls(x)}.new`];
        case "FinishWorker": return ["The worker's block returns; class L loses its owner", "# the worker's block ends"];
      }
    },
    RactorLocal(model, t) {
      const [c, k, kind] = t.args;
      const who = ractorName(model, c);
      const key = `:k${k}`;
      const v = kind === 1 ? "1" : "[1]";
      const other = c === 1 ? "worker" : "Ractor.main";
      switch (t.op) {
        case "Store": return [`${who} stores ${kind === 1 ? "a shareable" : "an unshareable"} value under ${key}`, `Ractor[${key}] = ${v}`];
        case "Fetch": return [`${who} reads ${key} from its own store`, `Ractor[${key}]`];
        case "StoreIfAbsent": return [`${who} stores under ${key} only if the key is unset`, `Ractor.store_if_absent(${key}) { ${v} }`];
        case "FetchOther": return [`${who} reads ${key} from the other Ractor's store`, `${other}[${key}]`];
        case "StoreOther": return [`${who} writes ${key} into the other Ractor's store`, `${other}[${key}] = ${v}`];
      }
    },
  };

  function describe(model, t) {
    const from = model.byId[t.from];
    const ctx = { ...model, aspect: from.fields.aspect };
    const [title, ruby] = say[model.name](ctx, t, from) || [t.op, ""];
    const sentences = [];
    const k = kind(t.outcome);
    if (k === "exit" && t.exit) {
      sentences.push(`The process exits with status ${t.exit.status}.`);
      sentences.push(t.exit.atExit ? "The at_exit handler runs." : "No at_exit handler runs.");
      const unwound = model.byId[t.to].fields.unwound;
      if (unwound) {
        const now = unwound.map((u, i) => u && !from.fields.unwound[i] ? `T${i + 1}` : null).filter(Boolean);
        sentences.push(now.length ? `${now.join(" and ")} run${now.length === 1 ? "s" : ""} ${now.length === 1 ? "its" : "their"} ensure clauses.` : "No thread unwinds.");
      }
      if (t.exit.stderr) sentences.push(`stderr shows "${t.exit.stderr}".`);
    } else {
      sentences.push(`The call ${resultText(model, t.op, t.outcome, t.value)}.`);
    }
    sentences.push(...wakesText(model, t));
    if (t.choice) sentences.push("This is a choice: from the same state, the same call may also end another way.");
    return { title, ruby, sentences, kind: k };
  }

  // ---------------------------------------------------------------- search

  // Breadth-first search from the initial states (of one aspect when given)
  // to the first transition t with test(t, fromState, model).  Returns [transitions].
  function shortest(model, test, aspect) {
    const starts = model.initial.filter((id) => !aspect || model.byId[id].fields.aspect === aspect);
    const parent = new Map(starts.map((id) => [id, null]));
    const queue = [...starts];
    while (queue.length) {
      const id = queue.shift();
      for (const t of model.out[id]) {
        if (test(t, model.byId[id], model)) {
          const path = [t];
          let cur = id;
          while (parent.get(cur)) { path.unshift(parent.get(cur)); cur = parent.get(cur).from; }
          return path;
        }
        if (!parent.has(t.to)) { parent.set(t.to, t); queue.push(t.to); }
      }
    }
    return null;
  }

  const has = (t, outcome) => (t.wakes || []).some((w) => w.outcome === outcome);

  const STORIES = {
    RactorPorts: [
      ["creator", "Only the creator receives", "A port belongs to the Ractor that made it. Another Ractor may send to it but not read from it.",
        (t) => t.op === "Receive" && t.outcome === "Ractor::Error:creator"],
      ["drain", "A closed port still hands out its messages", "Closing stops new messages. The ones already queued can still be received.",
        (t, s) => t.op === "Receive" && t.outcome === "ok" && !s.fields.portOpen[t.args[1] - 1]],
      ["selectforeign", "Ractor.select refuses a foreign port", "This rule was a bug in CRuby before this work: select read the caller's own port by mistake.",
        (t) => t.op === "Select" && t.outcome === "Ractor::Error:creator"],
    ],
    RactorBlocking: [
      ["wake", "A receive waits for a message", "The receiving thread sleeps until another Ractor sends.",
        (t) => t.op === "Send" && t.args[0] === 3 && has(t, "ok")],
      ["closewait", "Closing a port wakes the waiter with an error", "A thread that waits in Ractor.select must not wait forever for a port that can never deliver.",
        (t, s) => t.op === "Close" && has(t, "Ractor::ClosedError") && t.wakes.some((w) => s.fields.wait[w.thread - 1].op === "Select")],
      ["twowait", "Two waiters, one message", "Only one waiting thread gets the message. Which one is CRuby's choice.",
        (t) => t.op === "Send" && t.choice],
      ["raise", "Thread#raise interrupts a waiting receive", "The blocked call ends with the exception. Messages that arrive later stay queued.",
        (t) => t.op === "Raise"],
      ["wakeup", "Thread#wakeup is not enough", "A wakeup ends the sleep, not the call. The receive goes back to waiting.",
        (t) => t.op === "Wakeup"],
      ["finishkill", "When a worker ends, its waiting threads die", "The worker's other threads are killed inside their calls. Their ensure clauses run.",
        (t) => t.op === "FinishWorker" && has(t, "killed")],
      ["race", "Two threads want the worker's value", "One thread takes the value. The other is told it was already taken.",
        (t) => t.op === "Finish" && has(t, "Ractor::Error:taken")],
    ],
    RactorOrder: [
      ["fifo", "Messages keep their order, whoever sends them", "A port is a queue: the oldest message comes out first, from main or from any worker.",
        (t, s) => t.op === "Poll" && t.outcome === "ok" && s.fields.queue[t.args[0] - 1].length === 2 && s.fields.queue[t.args[0] - 1][0][0] !== s.fields.queue[t.args[0] - 1][1][0]],
      ["closeorder", "A closed port drains in order", "Close stops new messages but keeps the queue and its order.",
        (t, s) => t.op === "Poll" && t.outcome === "ok" && !s.fields.portOpen[t.args[0] - 1] && s.fields.queue[t.args[0] - 1].length === 2],
      ["selectchoice", "Select with two ready ports", "Ractor.select takes the oldest message of one ready port. The specification lets CRuby pick which.",
        (t) => t.op === "Select" && t.choice && t.args[0][0] === "port"],
      ["mixed", "Select over a port and a finished worker", "A finished worker counts as ready. If its value was already taken, winning means an error.",
        (t) => t.op === "Select" && t.outcome === "Ractor::Error:taken"],
    ],
    RactorProcess: [
      ["exit", "exit unwinds every waiting thread", "At exit, at_exit handlers run and threads that wait inside a call run their ensure clauses.",
        (t, s) => t.op === "MainExit" && s.fields.ts[0] === "blocked" && s.fields.ts[1] === "blocked" && s.fields.atExit],
      ["exitbang", "exit! skips all cleanup", "No at_exit handler and no ensure clause runs.",
        (t, s) => t.op === "MainExitBang" && s.fields.ts[0] === "blocked" && s.fields.ts[1] === "blocked" && s.fields.atExit],
      ["fork", "After fork, the child is alone", "Only the forking thread survives. The worker looks terminated; its value is nil once.",
        (t, s) => t.op === "Value" && t.outcome === "nil"],
      ["forkworker", "Only main may fork", "fork in another Ractor raises Ractor::IsolationError.",
        (t) => t.op === "ForkInWorker"],
      ["sigint", "Ctrl-C interrupts main's receive", "SIGINT raises Interrupt in main's main thread. Other waiting threads keep waiting.",
        (t, s) => t.op === "Interrupted" && s.fields.ts[0] === "blocked"],
    ],
    RactorTermination: [
      ["successor", "Only one Ractor gets the value", "The first Ractor that takes the value becomes its successor. Every other Ractor is refused.",
        (t) => t.op === "Value" && t.outcome === "Ractor::Error:successor"],
      ["token", "A monitor port reports the end", "Each registration sends one token when the target ends.",
        (t) => t.op === "ReceiveToken" && t.outcome === "ok" && t.value === "exited"],
      ["late", "A late monitor still hears about the end", "monitor on a finished Ractor returns false and sends the token at once.",
        (t) => t.op === "Monitor" && t.outcome === "false"],
      ["abortjoin", "join after an abort raises once", "The first join gets the exception. The next join of the same Ractor is told that the value was taken.",
        (t) => t.op === "Join" && t.outcome === "Ractor::Error:taken"],
    ],
    RactorTransfer: [
      ["move", "Moving makes the sender's object unusable", "After a move, main holds only a shell. Any method call on it raises Ractor::MovedError.",
        (t) => t.op === "Touch" && t.outcome === "Ractor::MovedError"],
      ["copy", "Sending copies an unshareable object", "The worker gets a distinct object. Main keeps its own.",
        (t, s, m) => t.op === "Receive" && t.outcome === "ok" && m.byId[t.to].fields.workerHas[0] === "mutable"],
      ["shared", "Frozen objects travel by reference", "Ractor.make_shareable freezes the whole graph. The worker then holds the same object.",
        (t, s, m) => t.op === "Receive" && t.outcome === "ok" && s.fields.aspect === "pair" && m.byId[t.to].fields.workerHas[0] === "shared"],
      ["thread", "A Thread cannot be sent", "A Thread can be neither copied nor moved. The send raises Ractor::Error.",
        (t) => t.op === "SendCopy" && t.outcome === "Ractor::Error"],
    ],
    RactorIsolation: [
      ["constwrite", "Only the owner writes a class's constants", "Class L belongs to the worker. Main may not assign its constants.",
        (t, s) => s.fields.aspect === "const" && t.op === "Set" && t.outcome === "Ractor::IsolationError" && t.args[0] === 1 && t.args[1] === 2],
      ["constread", "Unshareable constants stay with the owner", "Any Ractor may read a shareable constant. Only the owner may read an unshareable one.",
        (t, s) => s.fields.aspect === "const" && t.op === "Get" && t.outcome === "Ractor::IsolationError"],
      ["global", "Global variables belong to main", "A worker that reads a global variable raises Ractor::IsolationError.",
        (t) => t.op === "GlobalGet" && t.outcome === "Ractor::IsolationError"],
      ["readonly", "A class outlives its owner as read-only", "After the worker ends, no Ractor may change class L. Reading it still works.",
        (t, s) => s.fields.aspect === "readonly" && t.op === "Set" && t.outcome === "Ractor::IsolationError" && !s.fields.alive],
      ["proc", "Shareable procs refuse unshareable captures", "A shareable proc may only read outer variables that are shareable and never reassigned.",
        (t) => t.op === "ShareableProc" && t.outcome === "Ractor::IsolationError" && t.args[1] === 3],
    ],
    RactorLocal: [
      ["own", "Each Ractor has its own store", "The worker does not see what main stored under the same key.",
        (t, s) => t.op === "Fetch" && t.args[0] === 2 && t.outcome === "nil" && s.fields.stored[0][t.args[1] - 1] !== "unset"],
      ["first", "store_if_absent keeps the first value", "A second store_if_absent leaves the stored value as it is and returns it.",
        (t, s) => t.op === "StoreIfAbsent" && ![("unset"), t.args[2] === 1 ? "shareable" : "unshareable"].includes(s.fields.stored[t.args[0] - 1][t.args[1] - 1])],
      ["other", "Only the current Ractor reaches its store", "Reading another Ractor's store with Ractor#[] raises RuntimeError.",
        (t) => t.op === "FetchOther"],
    ],
  };

  function stories(model) {
    return (STORIES[model.name] || []).map(([id, title, why, test]) => ({ id, title, why, path: shortest(model, test) }))
      .filter((s) => s.path);
  }

  function choiceGroup(model, t) {
    return model.out[t.from].filter((o) => o.op === t.op && JSON.stringify(o.args) === JSON.stringify(t.args));
  }

  function rowKey(model, t) {
    return `${t.op}|${model.byId[t.from].fields.aspect}|${t.outcome}`;
  }

  function scope(model, paths) {
    const all = new Set(model.transitions.map((t) => rowKey(model, t)));
    const shown = new Set(paths.flat().map((t) => rowKey(model, t)));
    return { shown: shown.size, total: all.size };
  }

  // What one index of a tuple-valued variable stands for, by model.
  const INDEX_KINDS = {
    RactorPorts: { portOpen: "port", queue: "port" },
    RactorBlocking: { portOpen: "port", queue: "port", ts: "thread", wait: "thread" },
    RactorOrder: { portOpen: "port", queue: "port", sent: "ractor", alive: "ractor", taken: "ractor" },
    RactorProcess: { ts: "thread", unwound: "thread" },
    RactorTermination: { monitors: "port", tokens: "port" },
    RactorTransfer: { mainState: "object", shareable: "object", inbox: "object", workerHas: "object" },
    RactorIsolation: { slot: "class", slotSet: "class", defined: "class", included: "class", frozenClass: "class" },
    RactorLocal: { stored: "ractor" },
  };

  function hint(model, variable, key, aspect) {
    const kind = (INDEX_KINDS[model.name] || {})[variable];
    const cast = CAST[model.name];
    switch (kind) {
      case "port": return (cast.ports[key] || {}).label || null;
      case "thread": return (cast.threads[key] || {}).label || null;
      case "ractor": return cast.ractors[key] || null;
      case "class": return key === 1 ? "class K" : "class L";
      case "object": return (TRANSFER_NAMES[aspect] || [])[key - 1] || null;
      default: return null;
    }
  }

  function isSet(model, path) {
    return (model.sets || []).includes(path);
  }

  function fmt(value, path, model) {
    if (value === true) return "TRUE";
    if (value === false) return "FALSE";
    if (typeof value === "string") return `"${value}"`;
    if (Array.isArray(value)) {
      const inner = value.map((v) => fmt(v, `${path}.*`, model)).join(", ");
      return model && isSet(model, path) ? `{${inner}}` : `<<${inner}>>`;
    }
    if (value && typeof value === "object") {
      return `[${Object.entries(value).map(([k, v]) => `${k} |-> ${fmt(v, `${path}.${k}`, model)}`).join(", ")}]`;
    }
    return String(value);
  }

  // Tuples of up to four entries change entry by entry, as on the site's
  // lifecycle diagrams; other variables change as a whole.
  function diff(model, t) {
    const before = model.byId[t.from].fields;
    const after = model.byId[t.to].fields;
    const aspect = before.aspect;
    const out = [];
    for (const { name } of model.variables) {
      if (name === "aspect" || name === "step") continue;
      const a = before[name];
      const b = after[name];
      if (JSON.stringify(a) === JSON.stringify(b)) continue;
      if (Array.isArray(a) && Array.isArray(b) && a.length === b.length && a.length <= 4 && !isSet(model, name)) {
        a.forEach((v, i) => {
          if (JSON.stringify(v) === JSON.stringify(b[i])) return;
          out.push({ name: `${name}[${i + 1}]`, hint: hint(model, name, i + 1, aspect),
            before: fmt(v, `${name}.*`, model), after: fmt(b[i], `${name}.*`, model) });
        });
      } else {
        out.push({ name, hint: null, before: fmt(a, name, model), after: fmt(b, name, model) });
      }
    }
    return out;
  }

  // Names of the loaded API models that these helpers can draw and narrate,
  // in the order of the site's navigation.
  const ORDER = ["RactorPorts", "RactorTermination", "RactorTransfer", "RactorIsolation", "RactorLocal",
    "RactorBlocking", "RactorOrder", "RactorProcess"];
  function supported() {
    return ORDER.filter((n) => CAST[n] && (window.RACTOR_MODELS || {})[n]);
  }

  window.RX = { load, kind, scene, describe, shortest, stories, choiceGroup, supported, rowKey, scope, diff, fmt,
    threadName, ractorName, portName, CAST, COLORS };
})();
