---------------------------- MODULE RactorProcess ----------------------------
(***************************************************************************)
(* What happens to Ractors at the edges of the process: exit, fork, and    *)
(* signals.                                                                *)
(*                                                                         *)
(* Main's main thread (the driver) makes the process-level calls.  Thread  *)
(* 1 is another thread of main and thread 2 a thread of the worker.  A     *)
(* thread that is killed inside a call runs its ensure clauses: the model  *)
(* records it in `unwound`.                                                *)
(*                                                                         *)
(* exit: the process ends by returning from the main script, by exit, by   *)
(* an exception, or by exit! in main or in the worker.  The step that ends *)
(* the process carries `exit`: the exit status, whether at_exit handlers   *)
(* run, and the text that must appear on stderr.                           *)
(* fork: main forks; the replay continues in the child, where only the     *)
(* forking thread exists and the worker looks terminated.                  *)
(* signal: a USR1 handler of main counts its runs; SIGINT interrupts a     *)
(* receive in main's main thread.                                          *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

Threads == {1, 2}
Aspects == {"exit", "fork", "signal"}
Outcomes == {"ok", "nil", "blocks", "runs", "killed", "exit", "Interrupt",
             "Ractor::ClosedError", "Ractor::IsolationError", "Ractor::Error:taken"}
Ops == {"Init", "Block", "Spin", "SendPort", "Poll", "AtExit", "WorkerExit", "FinishWorker",
        "MainReturn", "MainExit", "MainRaise", "MainExitBang", "WorkerExitBang",
        "Fork", "Count", "SendWorker", "Join", "Value", "NewRactor", "ForkInWorker",
        "Kill", "Interrupted", "WorkerTrap"}
HandledBound == 2

VARIABLES
  aspect,
  side,     \* "parent", or "child" after Fork
  alive,    \* the worker has not terminated, as this process sees it
  ts,       \* each thread: "idle", "blocked", "running", or "dead"
  unwound,  \* the thread was killed inside a call and ran its ensure clauses
  queued,   \* messages queued on main's port P1
  atExit,   \* main registered an at_exit handler
  result,   \* the worker's value: "none" while it runs, "ready", "nil", "taken"
  handled,  \* runs of main's USR1 handler
  ended,    \* the process ended
  step      \* [op, args, outcome, value, wakes, exit]

vars == <<aspect, side, alive, ts, unwound, queued, atExit, result, handled, ended, step>>

NoWake == <<"", "">>
Unwoken == [t \in Threads |-> NoWake]
WakeSeq(wk) == SelectSeq([i \in 1..2 |-> [thread |-> i, outcome |-> wk[i][1], value |-> wk[i][2]]],
                         LAMBDA r : r.outcome /= "")
StepX(op, args, outcome, value, wk, exit) ==
  [op |-> op, args |-> args, outcome |-> outcome, value |-> value, wakes |-> WakeSeq(wk), exit |-> exit]
Step(op, args, outcome, value) == StepX(op, args, outcome, value, Unwoken, <<>>)
Exit(status, runsAtExit, stderr) == [status |-> status, atExit |-> runsAtExit, stderr |-> stderr]

Init ==
  /\ aspect \in Aspects
  /\ side = "parent"
  /\ alive = TRUE
  /\ ts = [t \in Threads |-> "idle"]
  /\ unwound = [t \in Threads |-> FALSE]
  /\ queued = 0
  /\ atExit = FALSE
  /\ result = "none"
  /\ handled = 0
  /\ ended = FALSE
  /\ step = Step("Init", <<>>, "ok", "")

Live == ~ended
InCall(t) == ts[t] \in {"blocked", "running"}
Idle(t) == ts[t] = "idle" /\ (t = 2 => alive)

-----------------------------------------------------------------------------
(* Thread 1 receives on P1; thread 2 receives on the worker's default     *)
(* port, which gets no message here.                                       *)
Block(t) ==
  /\ Live
  /\ Idle(t)
  /\ IF t = 1 /\ queued > 0
       THEN /\ queued' = queued - 1
            /\ step' = Step("Block", <<t>>, "ok", "msg")
            /\ UNCHANGED ts
       ELSE /\ ts' = [ts EXCEPT ![t] = "blocked"]
            /\ step' = Step("Block", <<t>>, "blocks", "")
            /\ UNCHANGED queued
  /\ UNCHANGED <<aspect, side, alive, unwound, atExit, result, handled, ended>>

(* Thread 2 runs a loop that never returns and never sleeps.               *)
Spin ==
  /\ Live
  /\ aspect = "exit"
  /\ Idle(2)
  /\ ts' = [ts EXCEPT ![2] = "running"]
  /\ step' = Step("Spin", <<2>>, "runs", "")
  /\ UNCHANGED <<aspect, side, alive, unwound, queued, atExit, result, handled, ended>>

(* The driver sends to P1; a thread that waits there takes the message.    *)
SendPort ==
  /\ Live
  /\ aspect \in {"fork", "signal"}
  /\ IF ts[1] = "blocked"
       THEN /\ ts' = [ts EXCEPT ![1] = "idle"]
            /\ step' = StepX("SendPort", <<>>, "ok", "", [Unwoken EXCEPT ![1] = <<"ok", "msg">>], <<>>)
            /\ UNCHANGED queued
       ELSE /\ queued = 0
            /\ queued' = 1
            /\ step' = Step("SendPort", <<>>, "ok", "")
            /\ UNCHANGED ts
  /\ UNCHANGED <<aspect, side, alive, unwound, atExit, result, handled, ended>>

Poll ==
  /\ Live
  /\ aspect = "fork"
  /\ queued' = 0
  /\ step' = IF queued > 0 THEN Step("Poll", <<>>, "ok", "msg") ELSE Step("Poll", <<>>, "nil", "")
  /\ UNCHANGED <<aspect, side, alive, ts, unwound, atExit, result, handled, ended>>

AtExit ==
  /\ Live
  /\ aspect \in {"exit", "fork"}
  /\ side = "parent"
  /\ ~atExit
  /\ atExit' = TRUE
  /\ step' = Step("AtExit", <<>>, "ok", "")
  /\ UNCHANGED <<aspect, side, alive, ts, unwound, queued, result, handled, ended>>

-----------------------------------------------------------------------------
(* The worker's main thread calls exit.  The worker aborts: its value      *)
(* raises Ractor::RemoteError caused by SystemExit, and thread 2 is killed *)
(* inside its call.  The process goes on.                                  *)
WorkerExit ==
  /\ Live
  /\ aspect = "exit"
  /\ alive
  /\ alive' = FALSE
  /\ ts' = [ts EXCEPT ![2] = "dead"]
  /\ unwound' = [unwound EXCEPT ![2] = @ \/ InCall(2)]
  /\ step' = StepX("WorkerExit", <<>>, "ok", "SystemExit",
                   [Unwoken EXCEPT ![2] = IF InCall(2) THEN <<"killed", "">> ELSE NoWake], <<>>)
  /\ UNCHANGED <<aspect, side, queued, atExit, result, handled, ended>>

(* The worker's block returns :finished; thread 2 is killed.               *)
FinishWorker ==
  /\ Live
  /\ aspect = "fork"
  /\ side = "parent"
  /\ alive
  /\ alive' = FALSE
  /\ result' = "ready"
  /\ ts' = [ts EXCEPT ![2] = "dead"]
  /\ unwound' = [unwound EXCEPT ![2] = @ \/ InCall(2)]
  /\ step' = StepX("FinishWorker", <<>>, "ok", "",
                   [Unwoken EXCEPT ![2] = IF InCall(2) THEN <<"killed", "">> ELSE NoWake], <<>>)
  /\ UNCHANGED <<aspect, side, queued, atExit, handled, ended>>

(* The process ends.  Return, exit, and an exception run the at_exit       *)
(* handlers and unwind every thread that is inside a call, in main and in  *)
(* a live worker.  exit! does neither.                                     *)
EndProcess(op, status, unwinds, stderr) ==
  /\ Live
  /\ ended' = TRUE
  /\ unwound' = [t \in Threads |-> unwound[t] \/ (unwinds /\ InCall(t) /\ (t = 2 => alive))]
  /\ ts' = [t \in Threads |-> "dead"]
  /\ step' = StepX(op, <<>>, "exit", "", Unwoken, Exit(status, unwinds /\ atExit, stderr))
  /\ UNCHANGED <<aspect, side, alive, queued, atExit, result, handled>>

MainReturn == /\ aspect \in {"exit", "fork"} /\ EndProcess("MainReturn", 0, TRUE, "")
MainExit == /\ aspect = "exit" /\ EndProcess("MainExit", 3, TRUE, "")
MainRaise == /\ aspect = "exit" /\ EndProcess("MainRaise", 1, TRUE, "main boom")
MainExitBang == /\ aspect = "exit" /\ EndProcess("MainExitBang", 4, FALSE, "")
WorkerExitBang == /\ aspect = "exit" /\ alive /\ EndProcess("WorkerExitBang", 5, FALSE, "")

-----------------------------------------------------------------------------
(* The driver forks.  The child has only the forking thread: thread 1 and  *)
(* the worker's threads are gone without unwinding.  The child sees the    *)
(* worker as terminated; if it ran at the fork, its value is nil once.     *)
Fork ==
  /\ Live
  /\ aspect = "fork"
  /\ side = "parent"
  /\ side' = "child"
  /\ alive' = FALSE
  /\ ts' = [t \in Threads |-> "dead"]
  /\ result' = IF alive THEN "nil" ELSE result
  /\ step' = Step("Fork", <<>>, "ok", "child")
  /\ UNCHANGED <<aspect, unwound, queued, atExit, handled, ended>>

Count ==
  /\ Live
  /\ aspect = "fork"
  /\ step' = Step("Count", <<>>, "ok", IF alive THEN "2" ELSE "1")
  /\ UNCHANGED <<aspect, side, alive, ts, unwound, queued, atExit, result, handled, ended>>

(* Sends to a terminated worker's default port fail.                       *)
SendWorker ==
  /\ Live
  /\ aspect = "fork"
  /\ ~alive
  /\ step' = Step("SendWorker", <<>>, "Ractor::ClosedError", "")
  /\ UNCHANGED <<aspect, side, alive, ts, unwound, queued, atExit, result, handled, ended>>

Join ==
  /\ Live
  /\ aspect = "fork"
  /\ ~alive
  /\ step' = Step("Join", <<>>, "ok", "self")
  /\ UNCHANGED <<aspect, side, alive, ts, unwound, queued, atExit, result, handled, ended>>

Value ==
  /\ Live
  /\ aspect = "fork"
  /\ ~alive
  /\ result' = "taken"
  /\ step' = CASE result = "ready" -> Step("Value", <<>>, "ok", "finished")
               [] result = "nil" -> Step("Value", <<>>, "nil", "")
               [] OTHER -> Step("Value", <<>>, "Ractor::Error:taken", "")
  /\ UNCHANGED <<aspect, side, alive, ts, unwound, queued, atExit, handled, ended>>

NewRactor ==
  /\ Live
  /\ aspect = "fork"
  /\ step' = Step("NewRactor", <<>>, "ok", "fresh")
  /\ UNCHANGED <<aspect, side, alive, ts, unwound, queued, atExit, result, handled, ended>>

(* Only main may fork.                                                     *)
ForkInWorker ==
  /\ Live
  /\ aspect = "fork"
  /\ Idle(2)
  /\ step' = Step("ForkInWorker", <<2>>, "Ractor::IsolationError", "")
  /\ UNCHANGED <<aspect, side, alive, ts, unwound, queued, atExit, result, handled, ended>>

-----------------------------------------------------------------------------
(* Process.kill(:USR1, Process.pid) from a thread of main or of the        *)
(* worker.  Main's handler runs; a thread blocked in receive stays there.  *)
Kill(t) ==
  /\ Live
  /\ aspect = "signal"
  /\ Idle(t)
  /\ handled < HandledBound
  /\ handled' = handled + 1
  /\ step' = Step("Kill", <<t>>, "ok", "")
  /\ UNCHANGED <<aspect, side, alive, ts, unwound, queued, atExit, result, ended>>

(* SIGINT while main's main thread waits in receive on an empty P1: the    *)
(* receive raises Interrupt.  Thread 1 keeps waiting.                      *)
Interrupted ==
  /\ Live
  /\ aspect = "signal"
  /\ queued = 0
  /\ step' = Step("Interrupted", <<>>, "Interrupt", "")
  /\ UNCHANGED <<aspect, side, alive, ts, unwound, queued, atExit, result, handled, ended>>

(* Signal.trap in the worker makes its handler shareable: a block that    *)
(* captures an unshareable object is refused (capture 2).                  *)
WorkerTrap(capture) ==
  /\ Live
  /\ aspect = "signal"
  /\ Idle(2)
  /\ step' = Step("WorkerTrap", <<2, capture>>, IF capture = 1 THEN "ok" ELSE "Ractor::IsolationError", "")
  /\ UNCHANGED <<aspect, side, alive, ts, unwound, queued, atExit, result, handled, ended>>

Next ==
  \/ \E t \in Threads : Block(t) \/ Kill(t)
  \/ Spin \/ SendPort \/ Poll \/ AtExit
  \/ WorkerExit \/ FinishWorker
  \/ MainReturn \/ MainExit \/ MainRaise \/ MainExitBang \/ WorkerExitBang
  \/ Fork \/ Count \/ SendWorker \/ Join \/ Value \/ NewRactor \/ ForkInWorker
  \/ Interrupted
  \/ \E c \in {1, 2} : WorkerTrap(c)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
  /\ aspect \in Aspects
  /\ side \in {"parent", "child"}
  /\ alive \in BOOLEAN
  /\ ts \in [Threads -> {"idle", "blocked", "running", "dead"}]
  /\ unwound \in [Threads -> BOOLEAN]
  /\ queued \in 0..1
  /\ atExit \in BOOLEAN
  /\ result \in {"none", "ready", "nil", "taken"}
  /\ handled \in 0..HandledBound
  /\ ended \in BOOLEAN
  /\ step.op \in Ops
  /\ step.outcome \in Outcomes

(* A child has no thread but the one that forked, and no live worker.     *)
ChildIsAlone ==
  side = "child" => ~alive /\ \A t \in Threads : ts[t] = "dead"

(* Only a thread that died inside a call unwound.                          *)
UnwoundMeansDead ==
  \A t \in Threads : unwound[t] => ts[t] = "dead"

(* A worker's thread runs only while the worker does.                      *)
NoThreadInADeadWorker ==
  ~alive => ts[2] = "dead"

-----------------------------------------------------------------------------
(* Action properties *)

(* exit! skips at_exit handlers and unwinds nothing.                       *)
BangSkipsCleanup ==
  [][step'.op \in {"MainExitBang", "WorkerExitBang"} => unwound' = unwound /\ ~step'.exit.atExit]_vars

(* Fork unwinds nothing in the child.                                      *)
ForkUnwindsNothing ==
  [][step'.op = "Fork" => unwound' = unwound]_vars

=============================================================================
