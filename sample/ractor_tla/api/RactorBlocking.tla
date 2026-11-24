---------------------------- MODULE RactorBlocking ----------------------------
(***************************************************************************)
(* Blocking calls and the events that end them.                            *)
(*                                                                         *)
(* Each Ractor runs modeled threads: threads 1 and 2 run in main, threads  *)
(* 3 and 4 in the worker.  A call that cannot return at once blocks its    *)
(* thread: the outcome of the step is "blocks" and the thread waits.  A    *)
(* later call by another thread, or the termination of the worker, ends    *)
(* the wait.  `step.wakes` lists every thread that the step woke, in       *)
(* thread order, with the outcome and value of its blocked call.           *)
(*                                                                         *)
(* A thread killed while it blocks reports "killed": its ensure clauses    *)
(* ran and its call never returned.                                        *)
(*                                                                         *)
(* Where CRuby may wake one of several waiters or take one of several      *)
(* ready ports, the model has one transition per choice.  The verifier     *)
(* replays the choice that CRuby makes and reports the others.             *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

Main == 1
Worker == 2
Threads == 1..4
RactorOf(t) == IF t <= 2 THEN Main ELSE Worker
Sibling(t) == CASE t = 1 -> 2 [] t = 2 -> 1 [] t = 3 -> 4 [] t = 4 -> 3

(* Port 1 is the worker's default port; ports 2 and 3 belong to main.      *)
D == 1
P1 == 2
P2 == 3
Ports == {D, P1, P2}
Owner == <<Worker, Main, Main>>
PortName == <<"D", "P1", "P2">>
QueueBound == 1

(* main: threads 1 and 2 block on main's ports, thread 3 sends to them.    *)
(* worker: threads 3 and 4 block on the worker's default port, thread 1    *)
(* sends to it.  exit, abort: threads 1 and 2 wait for the worker, which   *)
(* returns from its block or raises.                                       *)
Aspects == {"main", "worker", "exit", "abort"}
Active(a) == CASE a = "main" -> {1, 2, 3}
              [] a = "worker" -> {1, 3, 4}
              [] OTHER -> {1, 2}
UsedPorts(a) == CASE a = "main" -> {P1, P2}
                  [] a = "worker" -> {D}
                  [] OTHER -> {}
RecvPort(a) == IF a = "worker" THEN D ELSE P1
SelectPorts == {P1, P2}

WaitOps == {"none", "Receive", "ReceiveTimed", "Select", "Join", "Value", "SelectRactor", "ReceiveToken"}
Outcomes == {"ok", "nil", "blocks", "killed", "RuntimeError", "Ractor::ClosedError",
             "Ractor::RemoteError", "Ractor::Error:taken"}
Ops == {"Init", "Send", "Receive", "ReceiveTimed", "ReceiveShort", "Select", "Close",
        "Raise", "Wakeup", "Kill", "Join", "Value", "SelectRactor", "ReceiveToken",
        "FinishWorker", "Finish"}
Values == {"", "msg", "D", "P1", "P2", "self", "finished", "selected", "exited", "aborted"}

VARIABLES
  aspect,
  alive,    \* the worker has not terminated
  portOpen, \* portOpen[p] is FALSE once p was closed
  queue,    \* number of queued messages on each port
  ts,       \* each thread: "idle", "blocked", or "dead"
  wait,     \* the call each blocked thread waits in: [op, ports]
  taken,    \* main took the worker's value or its exception
  token,    \* a termination token waits on main's monitor port
  step      \* the last call: [op, args, outcome, value, wakes]

vars == <<aspect, alive, portOpen, queue, ts, wait, taken, token, step>>

NoWait == [op |-> "none", ports |-> {}]
WaitIn(op, ps) == [op |-> op, ports |-> ps]

(* wk[t] is <<outcome, value>> for a woken thread t, NoWake otherwise.     *)
NoWake == <<"", "">>
Unwoken == [t \in Threads |-> NoWake]
WakeSeq(wk) == SelectSeq([i \in Threads |-> [thread |-> i, outcome |-> wk[i][1], value |-> wk[i][2]]],
                         LAMBDA r : r.outcome /= "")
Step(op, args, outcome, value, wk) ==
  [op |-> op, args |-> args, outcome |-> outcome, value |-> value, wakes |-> WakeSeq(wk)]

Init ==
  /\ aspect \in Aspects
  /\ alive = TRUE
  /\ portOpen = [p \in Ports |-> TRUE]
  /\ queue = [p \in Ports |-> 0]
  /\ ts = [t \in Threads |-> "idle"]
  /\ wait = [t \in Threads |-> NoWait]
  /\ taken = FALSE
  /\ token = FALSE
  /\ step = Step("Init", <<>>, "ok", "", Unwoken)

Idle(t) == t \in Active(aspect) /\ ts[t] = "idle"
Blocked(t) == ts[t] = "blocked"
Waiters(p) == {u \in Threads : Blocked(u) /\ p \in wait[u].ports}
Ready(p) == queue[p] > 0
Drained(p) == ~portOpen[p] /\ queue[p] = 0
MsgValue(u, p) == IF wait[u].op = "Select" THEN PortName[p] ELSE "msg"

(* The woken threads leave their wait; the others keep theirs.            *)
Release(woken, newTs) ==
  /\ ts' = [u \in Threads |-> IF u \in woken THEN newTs ELSE ts[u]]
  /\ wait' = [u \in Threads |-> IF u \in woken THEN NoWait ELSE wait[u]]

BlockOn(t, op, ps, args) ==
  /\ ts' = [ts EXCEPT ![t] = "blocked"]
  /\ wait' = [wait EXCEPT ![t] = WaitIn(op, ps)]
  /\ step' = Step(op, args, "blocks", "", Unwoken)

-----------------------------------------------------------------------------
(* Port#send and Ractor#send.  A message for a port that some thread waits *)
(* on goes to one of the waiters and wakes it; otherwise it is queued.     *)
Send(t, p) ==
  /\ Idle(t)
  /\ p \in UsedPorts(aspect)
  /\ IF ~portOpen[p]
       THEN /\ step' = Step("Send", <<t, p>>, "Ractor::ClosedError", "", Unwoken)
            /\ UNCHANGED <<queue, ts, wait>>
     ELSE IF Waiters(p) /= {}
       THEN \E u \in Waiters(p) :
              /\ Release({u}, "idle")
              /\ step' = Step("Send", <<t, p>>, "ok", "", [Unwoken EXCEPT ![u] = <<"ok", MsgValue(u, p)>>])
              /\ UNCHANGED queue
     ELSE /\ queue[p] < QueueBound
          /\ queue' = [queue EXCEPT ![p] = @ + 1]
          /\ step' = Step("Send", <<t, p>>, "ok", "", Unwoken)
          /\ UNCHANGED <<ts, wait>>
  /\ UNCHANGED <<aspect, alive, portOpen, taken, token>>

(* receive without a timeout (Receive) and with a long one (ReceiveTimed): *)
(* a queued message returns at once, a closed and drained port raises,     *)
(* and an open empty port blocks the thread.                               *)
(* TLC names an edge after the outermost conjunction, so each action      *)
(* states its own guard and calls this shared body.                       *)
ReceiveLike(op, t) ==
  /\ LET p == RecvPort(aspect) IN
       /\ RactorOf(t) = Owner[p]
       /\ IF Ready(p)
            THEN /\ queue' = [queue EXCEPT ![p] = @ - 1]
                 /\ step' = Step(op, <<t, p>>, "ok", "msg", Unwoken)
                 /\ UNCHANGED <<ts, wait>>
          ELSE IF ~portOpen[p]
            THEN /\ step' = Step(op, <<t, p>>, "Ractor::ClosedError", "", Unwoken)
                 /\ UNCHANGED <<queue, ts, wait>>
          ELSE /\ BlockOn(t, op, {p}, <<t, p>>)
               /\ UNCHANGED queue
  /\ UNCHANGED <<aspect, alive, portOpen, taken, token>>

Receive(t) == /\ aspect \in {"main", "worker"} /\ Idle(t) /\ ReceiveLike("Receive", t)
ReceiveTimed(t) == /\ aspect \in {"main", "worker"} /\ Idle(t) /\ ReceiveLike("ReceiveTimed", t)

(* receive(timeout: 0.05) on an open empty port returns nil when the      *)
(* timeout passes.  No other action runs in the meantime.                 *)
ReceiveShort(t) ==
  /\ aspect \in {"main", "worker"}
  /\ Idle(t)
  /\ LET p == RecvPort(aspect) IN
       /\ RactorOf(t) = Owner[p]
       /\ IF Ready(p)
            THEN /\ queue' = [queue EXCEPT ![p] = @ - 1]
                 /\ step' = Step("ReceiveShort", <<t, p>>, "ok", "msg", Unwoken)
            ELSE /\ step' = Step("ReceiveShort", <<t, p>>,
                                 IF portOpen[p] THEN "nil" ELSE "Ractor::ClosedError", "", Unwoken)
                 /\ UNCHANGED queue
  /\ UNCHANGED <<aspect, alive, portOpen, ts, wait, taken, token>>

(* Ractor.select(P1, P2): a closed and drained port raises first; then one *)
(* ready port delivers, any of them when both are ready; else it blocks.  *)
Select(t) ==
  /\ aspect = "main"
  /\ Idle(t)
  /\ RactorOf(t) = Main
  /\ IF \E p \in SelectPorts : Drained(p)
       THEN /\ step' = Step("Select", <<t, P1, P2>>, "Ractor::ClosedError", "", Unwoken)
            /\ UNCHANGED <<queue, ts, wait>>
     ELSE IF \E p \in SelectPorts : Ready(p)
       THEN \E p \in {q \in SelectPorts : Ready(q)} :
              /\ queue' = [queue EXCEPT ![p] = @ - 1]
              /\ step' = Step("Select", <<t, P1, P2>>, "ok", PortName[p], Unwoken)
              /\ UNCHANGED <<ts, wait>>
     ELSE /\ BlockOn(t, "Select", SelectPorts, <<t, P1, P2>>)
          /\ UNCHANGED queue
  /\ UNCHANGED <<aspect, alive, portOpen, taken, token>>

(* Port#close and Ractor#close by a thread of the creator.  Every thread   *)
(* that waits on the port raises Ractor::ClosedError, a select waiter too. *)
Close(t, p) ==
  /\ Idle(t)
  /\ p \in UsedPorts(aspect)
  /\ RactorOf(t) = Owner[p]
  /\ portOpen' = [portOpen EXCEPT ![p] = FALSE]
  /\ Release(Waiters(p), "idle")
  /\ step' = Step("Close", <<t, p>>, "ok", "",
                  [u \in Threads |-> IF u \in Waiters(p) THEN <<"Ractor::ClosedError", "">> ELSE NoWake])
  /\ UNCHANGED <<aspect, alive, queue, taken, token>>

-----------------------------------------------------------------------------
(* Thread#raise, Thread#wakeup, and Thread#kill by the sibling thread of   *)
(* the same Ractor, on a thread that blocks.  Thread#raise returns nil.    *)
Interruptible(t) ==
  /\ Idle(t)
  /\ Sibling(t) \in Active(aspect)
  /\ Blocked(Sibling(t))

Raise(t) ==
  /\ Interruptible(t)
  /\ Release({Sibling(t)}, "idle")
  /\ step' = Step("Raise", <<t, Sibling(t)>>, "nil", "", [Unwoken EXCEPT ![Sibling(t)] = <<"RuntimeError", "">>])
  /\ UNCHANGED <<aspect, alive, portOpen, queue, taken, token>>

(* A wakeup ends the sleep, not the call: the thread waits again.          *)
Wakeup(t) ==
  /\ Interruptible(t)
  /\ step' = Step("Wakeup", <<t, Sibling(t)>>, "ok", "", Unwoken)
  /\ UNCHANGED <<aspect, alive, portOpen, queue, ts, wait, taken, token>>

Kill(t) ==
  /\ Interruptible(t)
  /\ Release({Sibling(t)}, "dead")
  /\ step' = Step("Kill", <<t, Sibling(t)>>, "ok", "", [Unwoken EXCEPT ![Sibling(t)] = <<"killed", "">>])
  /\ UNCHANGED <<aspect, alive, portOpen, queue, taken, token>>

-----------------------------------------------------------------------------
(* Waiting for the worker: Ractor#join, Ractor#value, Ractor.select on the *)
(* worker, and receive on a port that monitors it (registered at setup).   *)
Terminating == aspect \in {"exit", "abort"}
TokenKind == IF aspect = "exit" THEN "exited" ELSE "aborted"
TakeOutcome == IF aspect = "exit" THEN "ok" ELSE "Ractor::RemoteError"
TakeValue(op) == IF aspect = "abort" THEN "" ELSE IF op = "Value" THEN "finished" ELSE "selected"

(* join after an exit returns the worker and takes nothing; after an abort *)
(* it takes the exception like value does.                                 *)
Join(t) ==
  /\ Terminating
  /\ Idle(t)
  /\ IF alive
       THEN /\ BlockOn(t, "Join", {}, <<t>>)
            /\ UNCHANGED taken
     ELSE /\ UNCHANGED <<ts, wait>>
          /\ IF aspect = "exit"
               THEN /\ step' = Step("Join", <<t>>, "ok", "self", Unwoken)
                    /\ UNCHANGED taken
             ELSE IF ~taken
               THEN /\ taken' = TRUE
                    /\ step' = Step("Join", <<t>>, "Ractor::RemoteError", "", Unwoken)
             ELSE /\ step' = Step("Join", <<t>>, "Ractor::Error:taken", "", Unwoken)
                  /\ UNCHANGED taken
  /\ UNCHANGED <<aspect, alive, portOpen, queue, token>>

TakeLike(op, t) ==
  /\ IF alive
       THEN /\ BlockOn(t, op, {}, <<t>>)
            /\ UNCHANGED taken
     ELSE /\ UNCHANGED <<ts, wait>>
          /\ IF ~taken
               THEN /\ taken' = TRUE
                    /\ step' = Step(op, <<t>>, TakeOutcome, TakeValue(op), Unwoken)
               ELSE /\ step' = Step(op, <<t>>, "Ractor::Error:taken", "", Unwoken)
                    /\ UNCHANGED taken
  /\ UNCHANGED <<aspect, alive, portOpen, queue, token>>

Value(t) == /\ Terminating /\ Idle(t) /\ TakeLike("Value", t)
SelectRactor(t) == /\ Terminating /\ Idle(t) /\ TakeLike("SelectRactor", t)

(* The monitor port gets one token.  A receive after the worker ended and *)
(* its token was taken would block forever, so the model does not make it. *)
ReceiveToken(t) ==
  /\ Terminating
  /\ Idle(t)
  /\ alive \/ token
  /\ IF token
       THEN /\ token' = FALSE
            /\ step' = Step("ReceiveToken", <<t>>, "ok", TokenKind, Unwoken)
            /\ UNCHANGED <<ts, wait>>
       ELSE /\ BlockOn(t, "ReceiveToken", {}, <<t>>)
            /\ UNCHANGED token
  /\ UNCHANGED <<aspect, alive, portOpen, queue, taken>>

(* The worker returns from its block (exit) or raises (abort).  Every      *)
(* waiting join after an exit returns the worker.  One waiting taker gets  *)
(* the value or the exception, any of them; the others raise `taken`.      *)
(* One waiting receive on the monitor port gets the token, or it queues.   *)
Finish ==
  /\ Terminating
  /\ alive
  /\ alive' = FALSE
  /\ portOpen' = [portOpen EXCEPT ![D] = FALSE]
  /\ LET joiners == {u \in Threads : Blocked(u) /\ wait[u].op = "Join"}
         holders == {u \in Threads : Blocked(u) /\ wait[u].op \in {"Value", "SelectRactor"}}
         takers == IF aspect = "abort" THEN joiners \cup holders ELSE holders
         listeners == {u \in Threads : Blocked(u) /\ wait[u].op = "ReceiveToken"}
     IN \E first \in IF takers = {} THEN {0} ELSE takers :
        \E heard \in IF listeners = {} THEN {0} ELSE listeners :
          LET woken == joiners \cup holders \cup (listeners \cap {heard})
              Result(u) == IF u = first THEN <<TakeOutcome, TakeValue(wait[u].op)>>
                           ELSE IF u \in takers THEN <<"Ractor::Error:taken", "">>
                           ELSE IF u \in joiners THEN <<"ok", "self">>
                           ELSE <<"ok", TokenKind>>
          IN /\ Release(woken, "idle")
             /\ taken' = (takers /= {})
             /\ token' = (listeners = {})
             /\ step' = Step("Finish", <<>>, "ok", "",
                             [u \in Threads |-> IF u \in woken THEN Result(u) ELSE NoWake])
  /\ UNCHANGED <<aspect, queue>>

(* The worker's block returns while its threads block or idle: the worker *)
(* terminates, its default port closes, and every thread of it is killed. *)
FinishWorker ==
  /\ aspect = "worker"
  /\ alive
  /\ alive' = FALSE
  /\ portOpen' = [portOpen EXCEPT ![D] = FALSE]
  /\ LET woken == {u \in Threads : RactorOf(u) = Worker /\ Blocked(u)} IN
       /\ Release({u \in Threads : RactorOf(u) = Worker}, "dead")
       /\ step' = Step("FinishWorker", <<>>, "ok", "",
                       [u \in Threads |-> IF u \in woken THEN <<"killed", "">> ELSE NoWake])
  /\ UNCHANGED <<aspect, queue, taken, token>>

Next ==
  \/ \E t \in Threads, p \in Ports : Send(t, p) \/ Close(t, p)
  \/ \E t \in Threads :
       \/ Receive(t) \/ ReceiveTimed(t) \/ ReceiveShort(t) \/ Select(t)
       \/ Raise(t) \/ Wakeup(t) \/ Kill(t)
       \/ Join(t) \/ Value(t) \/ SelectRactor(t) \/ ReceiveToken(t)
  \/ Finish
  \/ FinishWorker

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
  /\ aspect \in Aspects
  /\ alive \in BOOLEAN
  /\ portOpen \in [Ports -> BOOLEAN]
  /\ queue \in [Ports -> 0..QueueBound]
  /\ ts \in [Threads -> {"idle", "blocked", "dead"}]
  /\ \A t \in Threads : wait[t].op \in WaitOps /\ wait[t].ports \subseteq Ports
  /\ taken \in BOOLEAN
  /\ token \in BOOLEAN
  /\ step.op \in Ops
  /\ step.outcome \in Outcomes
  /\ step.value \in Values
  /\ \A i \in DOMAIN step.wakes : step.wakes[i].outcome \in Outcomes \ {"blocks"}

(* A thread blocks exactly when it waits in a call.                        *)
BlockedMeansWaiting ==
  \A t \in Threads : (ts[t] = "blocked") <=> (wait[t].op /= "none")

(* Nobody waits on a port that could deliver or raise at once.            *)
WaitersHaveNothing ==
  \A t \in Threads : \A p \in wait[t].ports : queue[p] = 0 /\ portOpen[p]

(* No thread waits for a worker that has terminated, except on the        *)
(* monitor port after its one token was taken.                            *)
NoWaitForTheDead ==
  ~alive => \A t \in Threads : wait[t].op \notin {"Join", "Value", "SelectRactor"}

-----------------------------------------------------------------------------
(* Action properties *)

(* Only a blocked thread wakes, and a message wakes at most one.           *)
WakesOnlyBlocked ==
  [][/\ \A i \in DOMAIN step'.wakes : ts[step'.wakes[i].thread] = "blocked"
     /\ step'.op = "Send" => Len(step'.wakes) <= 1]_vars

(* At most one thread takes the worker's value or exception, once.        *)
TakenOnce ==
  [][/\ Cardinality({i \in DOMAIN step'.wakes :
                       step'.wakes[i].value \in {"finished", "selected"}
                       \/ step'.wakes[i].outcome = "Ractor::RemoteError"}) <= 1
     /\ taken => taken']_vars

DeadStaysDead ==
  [][\A t \in Threads : ts[t] = "dead" => ts'[t] = "dead"]_vars

=============================================================================
