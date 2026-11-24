# Ractor API: executable documentation of valid and invalid usage

Eight TLA+ modules describe the Ractor API as a state machine. Every API call
is an action whose `step` records the call and its outcome: `ok`, a value
class such as `nil`, or the class of the exception it raises. An invalid
call is a transition like any other, so each model also states what a
program sees after it rescues the error.

TLC checks every module exhaustively within its bounds. `verify_api.rb` then
replays every transition of every module against CRuby in a fresh process
through `api_harness.rb`. The specification therefore cannot drift from the
implementation without a failing run.

| Module | Section of `doc/language/ractor.md` | Cast |
| --- | --- | --- |
| `RactorPorts` | Communication, closing ports, `Ractor.select` | main, one worker, three ports |
| `RactorTermination` | Creation and termination | main, a terminating Ractor, an observer Ractor |
| `RactorTransfer` | Copy and move, shareable objects | main, one worker, a root and a leaf object |
| `RactorIsolation` | Language changes to limit sharing | main with class `K`, a worker with class `L` |
| `RactorLocal` | Ractor-local storage | main, one worker, two keys |
| `RactorBlocking` | Communication (blocking receive), closing ports, `join` and `value` | two threads in main, two threads in one worker |
| `RactorOrder` | Communication, `Ractor.select` | main and two workers that send numbered messages to two ports |
| `RactorProcess` | None: the document does not discuss exit, fork, or signals | main's main thread, one more thread in main, one thread in a worker |

Each module except `RactorPorts` and `RactorLocal` chooses an *aspect* in
`Init`. TLC explores the aspects as disjoint subgraphs, so one module covers
one section of the design document while every product of variables stays
small.

The last three modules model threads inside Ractors, blocking calls, choices
that CRuby makes, and the end of the process. "Threads, choices, and the
process" below describes the extra machinery.

## Result on this build

```
module RactorPorts states=2107 core=96 transitions=2266 replays=2219
module RactorTermination states=740 core=72 transitions=762 replays=728
module RactorTransfer states=5201 core=570 transitions=6074 replays=5793
module RactorIsolation states=586 core=69 transitions=674 replays=651
module RactorLocal states=1945 core=81 transitions=2592 replays=2553
module RactorBlocking states=1195 core=233 transitions=1434 replays=1354 choices=114 diverged=79 untaken=79
module RactorOrder states=2411 core=366 transitions=2592 replays=2392 choices=456 diverged=228 untaken=228
module RactorProcess states=648 core=166 transitions=701 replays=621
PASS modules=8 states=14833 transitions=17095 replays=16311 untaken=307
```

`states` counts TLC states, which include the last call. `core` counts
states after the verifier merges states that differ only in `step`. A
`transition` is one (core state, call) pair with its outcome. A `replay` is
one fresh Ruby process that performs a path of calls and compares every
outcome and every observable. All 16,311 replays pass on this build. The run
takes about two minutes with eight parallel jobs.

`choices` counts the transitions of calls that have more than one allowed
outcome. `diverged` counts the replays that stopped because CRuby took
another allowed outcome than the path expected. `untaken` counts the
transitions that no replay reached. In `RactorBlocking` the last two numbers
change from run to run: three runs gave 70, 79, and 73 untaken transitions.

## What the result proves

Every transition of every bounded model matches CRuby on this build: the
outcome class, the `Ractor::Error` rule named in the message, and the
observable state after the call (`closed?`, `alive?`, `frozen?`,
`Ractor.shareable?`, `const_defined?`, and the like).

Where a step names a `value`, the harness also compares what the call
returned: the message of a receive, the port and message of a select, the
kind of a termination token, the termination value, and the Ractor that
`join` returns. For a transfer, the worker reports for each object whether
it holds the sender's object itself or a distinct one, its frozen flag, and
whether the received graph kept its aliases or its cycle.

Most invariants restate the guards of the actions. They protect the
specification against edits. The CRuby replay is the evidence that CRuby
follows the rules.

It does not prove arbitrary programs, VM internals, unbounded queues, or
timing. `RactorPorts` and the first five modules make a blocking call only
where it returns at once; `RactorBlocking` models the wait itself.

An untaken transition is an outcome that the model allows and that CRuby did
not produce in any replay. The model does not require CRuby to produce it.
In the run above:

- `Ractor.select` with two ready ports returned the first argument, in both
  argument orders.
- `Ractor.select` over a terminated worker and a ready port returned the
  port, in both argument orders. The harness receives on its own ports
  between steps, and a receive moves pending messages into their port
  queues. The token of the terminated worker arrives during the call. This
  order is an artifact of delivery timing, not a rule. [INFERENCE: from
  `ractor_wait_receive` and `ractor_selector_scan`; no trace was taken.]
- `Ractor.select` over two terminated workers returned the first argument.
- When several threads waited, the thread that got the message or the value
  changed between runs.

## Outcome vocabulary

| Outcome | Meaning |
| --- | --- |
| `ok` | The call returned a value other than `nil`, `true`, or `false`. |
| `nil`, `true`, `false` | The call returned that value. |
| `Ractor::ClosedError`, `Ractor::MovedError`, `Ractor::RemoteError`, `Ractor::IsolationError`, `Ractor::UnsafeError`, `NameError`, `FrozenError`, `RuntimeError`, `Interrupt` | The call raised that class. |
| `Ractor::Error:creator` | "only allowed from the creator Ractor of this port" |
| `Ractor::Error:closer` | "closing port by other ractors is not allowed" |
| `Ractor::Error:taken` | "The value was already taken" |
| `Ractor::Error:successor` | "Only the successor ractor can take a value" |
| `blocks` | The call did not return: its thread sleeps inside it. |
| `runs` | The call did not return: its thread keeps running. |
| `killed` | The thread died inside the call, after its ensure clauses ran. |
| `exit` | The call ended the process; the step's `exit` record states the exit status, whether at_exit handlers ran, and the text on stderr. |

In the last three modules a step also lists `wakes`: every blocked thread
that the call woke, with the outcome and value of its own call.

## Rule matrix

Prose status: `stated` means `doc/language/ractor.md` or the method
documentation in `ractor.rb` says it; `silent` means neither does; `differs`
means the prose says something else. Every rule below is checked by TLC and
replayed on CRuby.

### Ports (`RactorPorts.tla`)

| Rule | Action | Outcome | Prose |
| --- | --- | --- | --- |
| Send to an open port never blocks. | `Send` | `ok` | stated |
| Send to a closed port is refused. | `Send` | `Ractor::ClosedError` | stated |
| Only the creator receives; the check precedes the closed check. | `Receive`, `Poll` | `Ractor::Error:creator` | stated; precedence silent |
| A closed port hands out its queued messages. | `Receive`, `Poll` | `ok` | stated |
| A closed, drained port raises on receive. | `Receive`, `Poll` | `Ractor::ClosedError` | stated |
| `receive(timeout: 0)` on an open empty port returns `nil`. | `Poll` | `nil` | stated |
| Only the creator closes; closing is idempotent and keeps the queue. | `Close` | `Ractor::Error:closer`, `ok` | stated |
| `Ractor#close` and `Port#close` return the port. | `Close` | `ok` | differs: `ractor.rb` documents `true` or `false` as the result of `Ractor#close` |
| `Ractor.select` rejects a port of another Ractor. | `Select`, `SelectPoll` | `Ractor::Error:creator` | silent; fixed in this change |
| A closed, drained port in `Ractor.select` raises even when another port is ready. | `Select`, `SelectPoll` | `Ractor::ClosedError` | stated; made deterministic in this change |
| A closed port with queued messages is ready for `Ractor.select`. | `Select`, `SelectPoll` | `ok` | silent |
| `Ractor.select(timeout: 0)` with nothing ready returns `nil`. | `SelectPoll` | `nil` | stated |
| A terminated Ractor's ports are closed; others still get the creator and closer errors first. | `FinishWorker` then any call | as above | stated; precedence silent |

### Termination (`RactorTermination.tla`)

| Rule | Action | Outcome | Prose |
| --- | --- | --- | --- |
| `monitor` while running registers and returns `true`; each registration yields one token, duplicates included. | `Monitor` | `true` | stated; duplicates silent |
| `monitor` after termination returns `false` and sends the token at once. | `Monitor` | `false` | stated |
| `unmonitor` removes every registration of the port and never raises. | `Unmonitor` | `ok` | partly stated |
| Termination sends `[ractor, :exited]` or `[ractor, :aborted]`; the harness compares the token. | `Finish`, `ReceiveToken` | `ok` | stated |
| `join` after an exit returns the Ractor to any caller, any number of times. | `Join` | `ok` (the Ractor) | stated |
| The first `value`, `Ractor.select(ractor)`, or `join` after an abort makes the caller the successor and hands out the value or `Ractor::RemoteError`. | `Value`, `SelectRactor`, `Join` | `ok`, `Ractor::RemoteError` | stated |
| The successor's next take reports the value as taken. | `Value`, `SelectRactor`, `Join` | `Ractor::Error:taken` | stated as `Ractor::Error` |
| Any other Ractor's take reports the successor rule. | `Value`, `SelectRactor`, `Join` | `Ractor::Error:successor` | stated as `Ractor::Error` |
| `join` after an abort consumes the exception: the same caller then gets `taken`, others `successor`. | `Join` | see above | silent |
| `Ractor.select(ractor, timeout: 0)` returns `nil` while the Ractor runs. | `SelectRactor` | `nil` | stated |

### Transfer (`RactorTransfer.tla`)

| Rule | Action | Outcome | Prose |
| --- | --- | --- | --- |
| A shareable object travels by reference: the worker holds the sender's object itself. `move: true` on it changes nothing for the sender. | `SendCopy`, `SendMove`, `Receive` | `ok`; `workerHas` = `shared` | stated |
| An unshareable object arrives as a distinct object, copied or moved, with its aliases, its cycle, and its frozen flag. A shareable object inside it is shared. | `SendCopy`, `SendMove`, `Receive` | `ok`; `workerHas` = `mutable`, `frozen`, `shared` | distinct object stated; aliases, frozen flag, and shared children silent |
| A move turns every reachable unshareable object into `Ractor::MovedObject`; any method on it raises. | `SendMove`, `Touch`, `Freeze` | `ok`, `Ractor::MovedError` | root stated; nested objects silent |
| A frozen container with a mutable child is unshareable and is moved like a mutable one. | aspect `frozen` | as above | silent |
| A `Thread` cannot be copied, moved, or made shareable. | `SendCopy`, `SendMove`, `MakeShareable`, `NewRactorArg` | `Ractor::Error` | differs: prose shows `TypeError` for the copy |
| `make_shareable` freezes every reachable object; `copy: true` leaves the original alone; it is idempotent. | `MakeShareable`, `MakeShareableCopy` | `ok` | stated |
| `Ractor.shareable?` answers `true` for a moved shell. | observable `shareable` | | silent |
| A shallow `freeze` does not make a container shareable. | `Freeze` | `ok` | stated |
| `Ractor.new(obj)` accepts the arguments that `send` accepts and refuses a `Thread`. | `NewRactorArg` | `ok`, `Ractor::Error` | stated |

### Isolation (`RactorIsolation.tla`)

| Rule | Action | Outcome | Prose |
| --- | --- | --- | --- |
| Only the owner writes constants, instance variables, and class variables of a class. | `Set` | `Ractor::IsolationError` | stated |
| Anyone reads a shareable value; only the owner reads an unshareable one; an unset constant or class variable raises `NameError`, an unset instance variable is `nil`. | `Get` | `ok`, `Ractor::IsolationError`, `NameError`, `nil` | stated |
| Only the owner defines, removes, or aliases methods, includes modules, or freezes the class. | `DefineMethod`, `RemoveMethod`, `AliasMethod`, `Include`, `FreezeClass` | `Ractor::IsolationError` | stated |
| Freezing a class that is already frozen is a no-op for any Ractor. | `FreezeClass` | `ok` | silent |
| On a frozen class, `const_set`, `class_variable_set`, and `dup` report ownership first; `instance_variable_set`, method changes, and `include` report `FrozenError` first. | `Set`, `DefineMethod` | `Ractor::IsolationError`, `FrozenError` | silent |
| Global variables belong to main; `$stderr` is Ractor-local. | `GlobalGet`, `GlobalSet`, `StderrGet` | `Ractor::IsolationError`, `ok` | stated |
| A C method of an extension not declared Ractor-safe raises outside main (`ObjectSpace.memsize_of`). | `CallUnsafe` | `Ractor::UnsafeError` | silent |
| `at_exit` and `Regexp.timeout=` are main-only. | `AtExit`, `RegexpTimeout` | `Ractor::IsolationError` | silent |
| A finalizer may be defined on own objects and own classes, not on a class of another Ractor. | `DefineFinalizer` | `ok`, `Ractor::IsolationError` | silent |
| `Ractor.shareable_proc` rejects captures of unshareable or reassigned outer variables and an unshareable `self`. | `ShareableProc` | `ok`, `Ractor::IsolationError` | stated |
| `Ractor.new` isolates its block by the same capture rules. | `NewRactor` | `ok`, `Ractor::IsolationError` | stated |
| `Class#dup` of a class created by another Ractor is refused. | `DupClass` | `Ractor::IsolationError` | differs: prose allows it unless constants or variables are unshareable |
| After its owner terminated a class is read-only for every Ractor; reading and instantiating stay allowed. | aspect `readonly` | `Ractor::IsolationError`, `ok` | stated |

### Local storage (`RactorLocal.tla`)

| Rule | Action | Outcome | Prose |
| --- | --- | --- | --- |
| `Ractor[key]` is `nil` for an unset key; any object may be stored. | `Store`, `Fetch` | `ok`, `nil` | stated |
| `Ractor.store_if_absent` keeps the first value and returns the stored one. | `StoreIfAbsent` | `ok` | stated |
| `other[key]` and `other[key] = value` raise for a non-current Ractor. | `FetchOther`, `StoreOther` | `RuntimeError` | stated |
| Each Ractor's store is invisible to the others. | observable `stored` | | stated |

### Blocking (`RactorBlocking.tla`)

| Rule | Action | Outcome | Prose |
| --- | --- | --- | --- |
| `receive` on an open empty port blocks; a send from another Ractor, or from another thread of the receiver, wakes it with the message. | `Receive`, `Send` | `blocks`, then a wake with `ok` | stated; a send by the same Ractor silent |
| `receive(timeout: 600)` waits and wakes like `receive`. | `ReceiveTimed` | `blocks` | stated |
| `receive(timeout: 0.05)` on an open empty port returns `nil` when the timeout passes. | `ReceiveShort` | `nil` | stated |
| `Ractor.select(P1, P2)` with both ports empty blocks; a send to either port wakes it with that port. | `Select`, `Send` | `blocks`, then a wake with `ok` | stated |
| Closing a port wakes every thread that waits on it with `Ractor::ClosedError`, a `receive` and a `Ractor.select` alike. | `Close` | wakes with `Ractor::ClosedError` | stated for `receive` in `Port#close`; stated for `Ractor.select` in this change |
| A message wakes one waiter only; when several threads wait, any of them may get it. | `Send` | a choice of wakes | silent |
| `Ractor.select` with both ports ready returns one of them. | `Select` | a choice of values | silent |
| `Thread#raise` on a blocked thread ends its call with that exception; a later message stays queued. `Thread#raise` returns `nil`. | `Raise` | `nil`, wake with `RuntimeError` | silent |
| `Thread#wakeup` does not end a blocked receive or select. | `Wakeup` | `ok`, no wake | silent |
| `Thread#kill` on a blocked thread kills it inside the call; its ensure clauses run. | `Kill` | wake with `killed` | silent |
| When the worker's block returns, its threads that block or idle are killed and its default port closes. | `FinishWorker` | wakes with `killed` | silent |
| Every `join` that waits for a worker that exits returns the worker; `join` takes nothing. | `Finish` | wakes with `ok` | stated |
| Of several threads that wait in `value`, `Ractor.select(worker)`, or (after an abort) `join`, one takes the value or the exception and the others raise `taken`. | `Finish` | a choice of wakes | stated per Ractor; per thread silent |
| A port that monitors the worker gets one token; one waiting receive gets it. | `Finish`, `ReceiveToken` | `ok` | stated |

### Order (`RactorOrder.tla`)

| Rule | Action | Outcome | Prose |
| --- | --- | --- | --- |
| A port hands out its messages in the order they were sent, whichever Ractor sent them, main included. | `Send`, `Poll` | `ok` with the oldest message | silent |
| A closed port hands out its queued messages in the same order, then raises. | `Close`, `Poll` | `ok`, `Ractor::ClosedError` | stated; the order silent |
| `Ractor.select` with both ports ready takes the oldest message of one of them, in either argument order. | `Select` | a choice of values | silent |
| `Ractor.select` over workers and ports: any ready source wins. A terminated worker is ready; when it wins and its value was taken, the call raises `taken`. | `Select` | a choice of values or `Ractor::Error:taken` | silent |
| `Ractor.select(..., timeout: 0)` sees a worker that terminated before the call. | `Select` | `ok` | silent |

### Process (`RactorProcess.tla`)

| Rule | Action | Outcome | Prose |
| --- | --- | --- | --- |
| When main returns, calls `exit`, or raises, the at_exit handlers run and every thread inside a call, in main and in a live worker, runs its ensure clauses. The exit status is 0, the given status, or 1. | `MainReturn`, `MainExit`, `MainRaise` | `exit` | silent |
| `exit!` in main or in a worker ends the process at once, without at_exit handlers or ensure clauses. | `MainExitBang`, `WorkerExitBang` | `exit` | silent |
| `exit` in a worker aborts only that worker: its value raises `Ractor::RemoteError` caused by `SystemExit`, and its threads unwind. | `WorkerExit` | `ok` | silent |
| A thread that runs without sleeping is unwound at exit like a blocked one. | `Spin`, `MainExit` | `runs`, `exit` | silent |
| The child of a fork has only the forking thread and sees every worker as terminated: `Ractor.count` is 1, `send` raises `Ractor::ClosedError`, and `join` returns the worker. | `Fork`, `Count`, `SendWorker`, `Join` | `ok`, `Ractor::ClosedError` | silent |
| In the child, `value` of a worker that ran at the fork returns `nil` once, then raises `taken`. | `Value` | `nil`, `Ractor::Error:taken` | silent |
| In the child, main's ports keep their queued messages, new Ractors work, and the at_exit handlers of the parent run at exit. | `Poll`, `NewRactor`, `MainReturn` | `ok`, `exit` | silent |
| `fork` in a worker raises `Ractor::IsolationError`. | `ForkInWorker` | `Ractor::IsolationError` | silent |
| A signal that any Ractor sends runs main's handler; a thread blocked in `receive` stays blocked. | `Kill` | `ok` | silent |
| SIGINT raises `Interrupt` in a `receive` of main's main thread; another blocked thread of main keeps waiting. | `Interrupted` | `Interrupt` | silent |
| `Signal.trap` in a worker accepts only a block that can be made shareable. | `WorkerTrap` | `ok`, `Ractor::IsolationError` | silent |

## Observed but not modeled

These were seen while probing CRuby. They are outside the models because
they involve degenerate inputs; each is a candidate for a separate fix.

- `Ractor.make_shareable` on a graph that holds a moved shell fails with
  `NoMethodError` for `inspect` while it formats its own error message.
- A moved shell can be sent again; `Ractor::Port#send` treats it as a
  shareable reference and delivers the inaccessible shell.
- `ObjectSpace.define_finalizer` on a frozen object of another Ractor raises
  `FrozenError` before the ownership check.
- `Thread::Queue` refuses copying with `TypeError: can't dump Thread::Queue`
  through the marshal path, not with `Ractor::Error`.

`RactorProcess` models one more observation that may be a defect: in the
child of a fork, `value` of a worker that ran at the fork returns `nil`. The
worker never returned `nil`. A `Ractor::ClosedError` or a
`Ractor::RemoteError` would tell the program what happened.

## Changes to CRuby in this work

`ractor_sync.c` gained two fixes, both covered by `test/ruby/test_ractor.rb`:

- `ractor_selector_add` rejects a port created by another Ractor with
  `Ractor::Error`, like `Ractor::Port#receive`. Before, `Ractor.select`
  looked the port up by id in the caller's own table and returned a message
  of the caller's port that happened to share the id
  (`test_select_rejects_foreign_port`).
- `ractor_selector__wait` scans for closed, drained ports before it takes a
  message, so the outcome no longer depends on whether a pending message had
  been moved into its port queue by an earlier `close` or `receive`
  (`test_select_closed_port_wins`).

Upstream `ruby/ruby` master had the same selector code when this was written.

## Modeling bounds

- One message kind and a queue bound of one per port in `RactorPorts` and
  `RactorBlocking`.
- Two registrations and two tokens per monitor port in `RactorTermination`.
- One object in flight in `RactorTransfer`; graphs that hold a moved shell
  are not sent, frozen, or made shareable.
- Two Ractors in most modules, three in `RactorTermination` and
  `RactorOrder`.
- Two threads per Ractor in `RactorBlocking`, one modeled thread per Ractor
  besides main's main thread in `RactorProcess`.
- In `RactorOrder`, three messages in total, at most two per sender and two
  per port; only one worker sends to the second port, and only the first
  port is closed.
- `RactorPorts` disables `Ractor.select` when more than one port is ready;
  `RactorBlocking` and `RactorOrder` model that choice.
- `RactorBlocking` does not let a thread wait on the monitor port after its
  one token was taken: that wait would never end.
- One fork per path; the replay continues in the child.
- No scheduling, GC, `Ractor::Selector` (it is `:nodoc:`), `require`
  isolation, class variables through inheritance, or capture kinds beyond
  the six listed.

## How the verifier works

1. TLC checks each module with `-tool -workers 1 -fp 0 -seed 1`, dumps the
   state graph as DOT, and `tlc_tools.rb` parses the dump as a versioned
   input contract (see `../README.md`). `RactorProcess` is checked with
   `-deadlock`: a state after the process ended has no successor, and TLC
   leaves such states out of the DOT rank footer.
2. The verifier merges states that differ only in `step`. Each remaining
   (state, call) pair is one transition; the same pair must always lead to
   the same state, or the verifier reports nondeterminism. A call with
   several transitions from one state, which differ in outcome, value, or
   wakes, is a choice.
3. A breadth-first forest from the initial states yields one path per leaf
   and one per non-tree transition. Every transition is on at least one path.
   The forest grows in layers by the number of choices on the route, so a
   path crosses a choice only where no other route exists.
4. Each path becomes a JSON file with the calls, the expected outcomes, the
   expected return values where the model states one, the other outcomes of
   each choice, and the expected observables. `api_harness.rb` replays it in
   a fresh Ruby process. Workers act only on command through a private
   control port and answer on a main-owned event port. These instrumentation
   ports, the ready handshake, and a termination monitor registered last are
   not modeled.
5. The per-module counts are pinned in `verify_api.rb`. A changed count means
   the model changed; inspect it before updating the pin.

## Threads, choices, and the process

- **Agents.** A modeled thread is a Ruby thread of its Ractor that takes
  calls from a `Thread::Queue`. It reports each result on a main-owned wake
  port. If it dies inside a call, its ensure clause reports `killed`. A
  call is `blocks` when the thread sleeps inside it for a short grace
  period. A call is `runs` when the thread stays runnable.
- **Wakes.** After each step the harness waits until the threads that
  returned match the wakes of the step or of one of its alternatives. Then
  it waits a grace period for late wakes. Every thread that the model keeps
  blocked must still sleep inside its call. After the last step the harness
  checks once more for late returns.
- **Choices.** When CRuby produces one of the step's `alternatives`, the
  harness prints `api_harness: DIVERGED ... step=N chose=...` and stops. The
  verifier counts the transitions before step N and the alternative that
  CRuby took as reached.
- **Observables.** The harness polls the observables until they equal the
  model or a timeout passes. A woken thread first runs, then idles; a
  signal handler runs later than the signal.
- **Process.** `RactorProcess` replays run in a child process. The harness
  compares its exit status, its stdout lines in any order, and its stderr
  with the model. Each thread that unwinds prints `unwound N`, and the
  at_exit handler prints `at_exit`. A path that does not end the process
  ends with `exit!`, so only the PASS line and the threads unwound during
  the path appear. After `Fork`, the parent waits for the child. The parent
  then checks that its worker and threads did not change, and exits with the
  child's status.

## Workflow

Requirements are those of the worker-pool sample: a Ruby built from this
checkout, the default collector, Java 11 or newer, and TLA+ Tools v1.7.4
(`../README.md` lists the download, size, and SHA-1).

```sh
cd build
TLA2TOOLS_JAR=/absolute/path/tla2tools.jar \
  ../tool/runruby.rb --srcdir=.. --archdir=. -- \
  ../sample/ractor_tla/api/verify_api.rb
```

One module:

```sh
cd build
TLA2TOOLS_JAR=/absolute/path/tla2tools.jar \
  ../tool/runruby.rb --srcdir=.. --archdir=. -- \
  ../sample/ractor_tla/api/verify_api.rb --module=RactorTermination
```

Environment: `RACTOR_TLA_JOBS` sets the number of parallel replay processes
(default: the smaller of the core count and 8); `RACTOR_TLA_TIMEOUT_SCALE`
multiplies every timeout; `RUBY` and `JAVA_HOME` select the executables;
`RACTOR_TLA_UNTAKEN=1` lists the transitions that no replay reached.

On failure the verifier prints the module, the aspect, the replay index, the
step list with expected outcomes, and the child's stdout and stderr, for
example:

```
FAIL: RactorPorts aspect ports replay 61 exited with 1
steps:
  Close[2, 2] -> ok
stdout:
stderr:
.../api_harness.rb:124:in 'block in RactorAPIHarness::Driver#run': step 0 Close[2, 2]: expected ok, observed Ractor::Error:closer ("closing port by other ractors is not allowed") (RactorAPIHarness::Failure)
```

The troubleshooting table in `../README.md` applies to this verifier too.
