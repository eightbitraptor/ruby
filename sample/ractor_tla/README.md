# Ractor worker pool: TLA+ model and CRuby replay

This sample verifies one bounded Ractor worker-pool protocol in two steps.
TLC checks the TLA+ model exhaustively. `verify.rb` then replays every
complete schedule of the model's state graph against the Ruby program in a
fresh CRuby process, and runs one concurrent large-heap execution.

| File | Role |
| --- | --- |
| `RactorPool.tla` | Protocol model: two workers, three jobs, one FIFO, one result port per worker |
| `RactorPool.cfg` | TLC configuration: constants, eight invariants, two temporal properties |
| `ractor_pool.rb` | Ruby protocol with `--mode=free` and `--mode=replay --schedule=...` |
| `verify.rb` | Single entry point: TLC run, DOT parsing, stress run, 80 serial replays |
| `tlc_tools.rb` | Shared TLC runner, tool-message checker, DOT parser, and child-process helpers |
| `api/` | Eight TLA+ modules that document valid and invalid Ractor API usage, blocking calls, message order, and exit, fork, and signals, with their own verifier; see `api/README.md` |
| `site/` | Static site builder that explains Ractors from the models; see "Explanatory site" below |

## Verification boundary

TLC checks the bounded protocol: job conservation, unique assignment, result
integrity, transfer ownership, collection accounting, shutdown safety, and
eventual completion and observation under weak fairness. `QueueBound` holds
too, but its one-ready-result part restates the guard of `Process`. It is a
restriction of the model, not a property that TLC derives. The state graph
has 829 states, 828 tree edges, and 80 complete schedules of 17 tokens.

Schedule replay checks this public protocol against CRuby at the modeled
boundaries. Each replayed action happens at its modeled position, and every
local precondition is checked before the action runs. Free stress mode relaxes
the one-ready-result rule and is not a replayed model schedule.

The result does not prove arbitrary Ruby programs or VM-internal
synchronization. It does not cover scheduler placement, courier internals,
finalizers, compaction, `Ractor::Selector`, or the exact selection order when
several ports are ready at once.

Public APIs outside this scenario: `timeout:` arguments, `unmonitor`,
shareable callables (`Ractor.shareable_proc`, `Ractor.shareable_lambda`),
`Ractor.store_if_absent`, `require` isolation, and direct Ractor selection
(`Ractor.select(ractor)`). `Ractor.yield`, `Ractor#take`, and
`Ractor.receive_if` do not exist in this checkout. The modules in `api/`
cover all of these except `Ractor.shareable_lambda` and `require`. They
also cover blocking calls, message order, the choice among ready sources,
and exit, fork, and signals.

## Feature matrix

| Behavior | Model | Replay | Stress | Runtime probe |
| --- | --- | --- | --- | --- |
| `Ractor.new` with arguments and names | setup before `Init` | yes | yes | |
| Default port send and blocking `Ractor.receive` | `Dispatch`, `Receive` | immediate receive | waiting receive | |
| Explicit `Ractor::Port` per worker and `Ractor.select` | `Process`, `Collect` | one ready port | any ready port | |
| Shareable reference, `move: true` on a shareable object | job 1 | yes | yes | |
| Default deep copy: snapshot, aliases, cycle, shared child | job 2 | yes | yes | |
| Move: `Ractor::MovedObject`, `Ractor::MovedError`, preserved graph | job 3 | yes | yes | |
| Ractor-local storage (`Ractor.[]`, `Ractor.[]=`) | `processedBy` | yes | yes | |
| Per-Ractor object visibility (`ObjectSpace.each_object`) | | yes | yes | |
| Local major collection counters | `localCollections` | yes | concurrent | |
| Process statistics (`GC.stat(process: true)`) | | yes | yes | |
| One global collection | `GlobalCollect` | yes | yes | |
| Value absorption leaves process statistics unchanged | | | | regression probe |
| Default port close, `closed?`, late `Ractor::ClosedError` | `Stop` | yes | yes | |
| Monitor `[ractor, :exited]`, `join`, `alive?`, `value`, second `value` | `Observe` | yes | yes | |
| Control port closed after termination, `Ractor.count` baseline | | yes | yes | |
| Queued message survives `close`; drained port raises | | | | after free run |
| Aborted worker: `[ractor, :aborted]`, `Ractor::RemoteError` and cause | | | | after free run |

The absorption assertion is a runtime regression probe for this build. It is
not a modeled behavior and not a portable GC contract.

## Protocol

Main owns the FIFO `<<1, 2, 3>>`, one event port, one monitor port, and one
result port per worker. Each worker owns its default port and one private
control port. Control ports release replay boundaries and request statistics.
They are instrumentation and do not appear in the TLA+ state.

Job 1 is a nested shareable configuration sent with `move: true`. Job 2 is a
mutable graph sent by deep copy. Job 3 is the same graph shape sent with
`move: true`. Every job allocates one retained heap of `HeapCell` objects and
one temporary heap of `GarbageCell` objects, then runs one local major
collection. Replays use 32 objects per heap. The stress run uses
`HEAP_OBJECTS` objects per heap, 50,000 by default.

Schedule tokens: `11`/`12` dispatch, `21`/`22` receive, `31`/`32` process,
`41`/`42` collect, `50` global collection, `61`/`62` stop, `71`/`72` observe.
Main dispatches whenever a pending job and an idle worker exist. A worker
publishes a result only while no other result is ready, so each replayed
`Ractor.select` has exactly one ready source. Every complete schedule ends with
`50,61,71,62,72`.

## Requirements

- A Ruby built from this checkout and run through `tool/runruby.rb`. The
  protocol needs `GC.start(global:)`, `GC.stat(process:)`, and the
  `global_gc_count` statistics. Released Rubies can lack these keywords.
- The default collector with `GC.stress` disabled. `GC.stat(process: true)`
  has collector-specific support.
- Java 11 or newer. `verify.rb` uses `$JAVA_HOME/bin/java` when `JAVA_HOME`
  is set, otherwise `java` from `PATH`.
- TLA+ Tools [v1.7.4](https://github.com/tlaplus/tlaplus/releases/tag/v1.7.4).
  Download `tla2tools.jar` from
  <https://github.com/tlaplus/tlaplus/releases/download/v1.7.4/tla2tools.jar>
  outside the repository. `verify.rb` requires size `2274532` and SHA-1
  `bee4a54f3ee3d4afc347c3240ec2d9e93b075104`. It never downloads anything.

## Workflow

Build the checkout once:

```sh
./autogen.sh
mkdir -p build
cd build
../configure
make -j
cd ..
```

Run the complete verification:

```sh
cd build
TLA2TOOLS_JAR=/absolute/path/tla2tools.jar \
  ../tool/runruby.rb --srcdir=.. --archdir=. -- \
  ../sample/ractor_tla/verify.rb
```

Expected output:

```
PASS tlc_states=829 schedules=80 replays=80 stress_heap_objects=50000
```

Run only the concurrent stress mode, without Java or TLC:

```sh
cd build
HEAP_OBJECTS=200000 \
  ../tool/runruby.rb --srcdir=.. --archdir=. -- \
  ../sample/ractor_tla/verify.rb --stress-only
```

Expected output:

```
PASS stress_heap_objects=200000
```

Run one schedule by hand:

```sh
cd build
../tool/runruby.rb --srcdir=.. --archdir=. -- \
  ../sample/ractor_tla/ractor_pool.rb --mode=replay \
  --schedule=11,12,21,31,41,11,22,32,42,21,31,41,50,61,71,62,72
```

`verify.rb` keeps TLC metadata, the DOT dumps, the Java temporary directory,
and all child output in one temporary directory that it removes at exit. It
writes nothing into the repository.

### TLC invocation

```
java -Djava.io.tmpdir=TMP -XX:+UseParallelGC -Xmx512m -jar tla2tools.jar \
  -tool -workers 1 -fp 0 -seed 1 -metadir TMP/metadir \
  -config RactorPool.cfg -dump dot,actionlabels TMP/RactorPool.dot RactorPool.tla
```

`verify.rb` requires exit status zero, tool messages `2193` and `2199`, and a
final message `2186`. It rejects message classes `ERROR`, `TLCBUG`, and
`WARNING`, and codes `2110`, `2114`, `2121`, `2217`, and `2102`. The DOT dump
is parsed as a versioned input contract: fixed header lines, one initial node,
labeled black action edges, one dashed stuttering self-loop per state (TLC adds
it under liveness checking), `Terminal` self-loops only on `done` states, and
unordered `{rank = same; ...}` footer lines. Every other line shape is an
error. Schedules are read from the `history` variable of each `done` state.

## Explanatory site

`site/build_site.rb` writes a static HTML site that explains the Ractor API
from these models. It runs TLC on all nine modules and replays every
transition and schedule on CRuby, like `verify_api.rb` and `verify.rb`.
It writes the site only when every check passes.

Each module page starts with highlights: a few chosen paths through the
state graph, animated, with a caption, the Ruby call, the TLA+ action, and
the variables that each step changes. A line states how many of the
model's outcome rows the highlights show. Below them the page has the state
variables, one lifecycle diagram per variable, every action with its Ruby
call and outcome table, a shortest witness trace per outcome, the checked
guarantees, and the full state graph. Every outcome row has a Play button
that animates its witness in a player docked to the corner of the window.
While a path plays, the lifecycle diagrams and the full state graph mark
the current state and the last step. The overview page shows the
model checking and the replay of one model as an animation. The pool page
plays each of the 80 schedules as swimlanes, and the `RactorProcess` page
draws the timeline of a fork. The rules page shows each rule as an animated
card and keeps the full rule matrix from `api/README.md` below the cards.
The explorer page lets the reader choose every step. The site also has a
TLA+ primer.

The animations are widgets in `site/explore/`. `widgets.js` mounts every
element with a `data-rx-widget` attribute. `common.js` turns a state of any
API model into a scene, a plain-English caption, and Ruby. `model_data.rb`
writes the state graphs that the widgets read into `rx/data/`. `lifecycle.js`
marks the diagrams: each diagram lists the SVG node of every state in
`data-rx-nodes`, and the player announces each state with an `rx:state`
event. The other
pages in `site/explore/` are stand-alone prototypes of the same widgets;
`explore/export_data.rb` writes their data.

Graphviz `dot` must be on `PATH`.

```sh
cd build
TLA2TOOLS_JAR=/absolute/path/tla2tools.jar \
  ../tool/runruby.rb --srcdir=.. --archdir=. -- \
  ../sample/ractor_tla/site/build_site.rb --out=/tmp/ractor-site
```

Expected output: `PASS site=/tmp/ractor-site pages=13`. `primer.html` and
`style.css` are static inputs. `markdown.rb` renders the README sections.

## Troubleshooting

| Symptom | Cause and action |
| --- | --- |
| `FAIL: TLA2TOOLS_JAR must point to tla2tools.jar v1.7.4 ...` | Set `TLA2TOOLS_JAR` to the downloaded jar, or pass `--stress-only`. |
| `FAIL: ... has SHA-1 ..., expected bee4a54f...` or a size mismatch | The jar is not the v1.7.4 release asset. Download it again from the release page. |
| `FAIL: cannot run java` or `Java 11 or newer is required` | Install a JDK or JRE 11+, or set `JAVA_HOME`. |
| `ractor_pool.rb ... the default GC is required` or `GC.stat(process:) is required` | Run through `tool/runruby.rb` with a Ruby built from this checkout and the default collector. MMTk and wbcheck builds are not supported. |
| `GC.stress must be disabled` | `GC.stress` was set before the sample ran. Run the sample in a plain Ruby process without stress mode. |
| `FAIL: TLC timed out` or `replay N (...) timed out` | The host is slow or loaded. Set `RACTOR_TLA_TIMEOUT_SCALE=3` to multiply every timeout. |
| `FAIL: DOT has N nodes, expected 829` or `found N complete schedules, expected 80` | The model changed or TLC's output format changed. Inspect the model before you update either count. |
| `FAIL: unknown DOT line` | A different TLC version wrote the dump. Use the pinned v1.7.4 jar. |
| `replay N (...) exited with 1` with `precondition is false` | The Ruby driver and the model disagree. Compare `ractor_pool.rb` with the action in `RactorPool.tla`. |
| `process deltas before global collection: ...` | Another collection ran between the baseline and the check. Keep `GC.disable` in main and in every worker. |
