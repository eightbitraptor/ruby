---------------------------- MODULE RactorPorts ----------------------------
(***************************************************************************)
(* Valid and invalid use of Ractor::Port: send, receive, close, and        *)
(* Ractor.select, for one main Ractor and one worker Ractor.               *)
(*                                                                         *)
(* Every API call is an action whose `step` records the call and its       *)
(* outcome: "ok", "nil", or the class of the exception it raises.  An      *)
(* invalid call is a transition like any other, so the model also states   *)
(* what the program sees after it rescues the error.                       *)
(*                                                                         *)
(* Ports: 1 is the worker's default port (reached through Ractor#send,     *)
(* Ractor.receive, Ractor#close), 2 and 3 are explicit ports of main.      *)
(* Each port holds at most QueueBound messages; this is a bound of the     *)
(* model, not a rule of the API.                                           *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

Main == 1
Worker == 2
Ractors == {Main, Worker}

D == 1
P1 == 2
P2 == 3
Ports == {D, P1, P2}
Owner == <<Worker, Main, Main>>

QueueBound == 1

(* Caller first, then the selected ports.  A set with a port of another   *)
(* Ractor documents the creator rule of Ractor.select.                     *)
SelectArgs == {<<Main, P1, P2>>, <<Main, P1, D>>, <<Worker, D, P1>>}

(* Ractor::Error outcomes carry the rule that raised them, read from the    *)
(* exception message: `creator` for a receive or select by another Ractor, *)
(* `closer` for a close by another Ractor.                                 *)
Outcomes == {"ok", "nil", "Ractor::Error:creator", "Ractor::Error:closer", "Ractor::ClosedError"}
Ops == {"Init", "Send", "Receive", "Poll", "Close", "Select", "SelectPoll", "FinishWorker"}

(* What an ok receive or select returns: "msg" for a receive, the name of  *)
(* the delivering port for a select, "" when the model states no value.    *)
PortName == <<"D", "P1", "P2">>
Values == {"", "msg", "D", "P1", "P2"}

VARIABLES
  aspect,      \* this module has one aspect
  workerAlive, \* the worker Ractor has not terminated
  portOpen,    \* portOpen[p] is FALSE once p was closed
  queue,       \* number of queued messages on each port
  step         \* the last call: [op, args, outcome, value]

vars == <<aspect, workerAlive, portOpen, queue, step>>

StepV(op, args, outcome, value) == [op |-> op, args |-> args, outcome |-> outcome, value |-> value]
Step(op, args, outcome) == StepV(op, args, outcome, "")

Init ==
  /\ aspect = "ports"
  /\ workerAlive = TRUE
  /\ portOpen = [p \in Ports |-> TRUE]
  /\ queue = [p \in Ports |-> 0]
  /\ step = Step("Init", <<>>, "ok")

Alive(c) == c = Main \/ workerAlive

Unchanged == UNCHANGED <<aspect, workerAlive, portOpen, queue>>

-----------------------------------------------------------------------------
(* Ractor::Port#send / Ractor#send: never blocks; a closed port refuses.    *)
Send(c, p) ==
  /\ Alive(c)
  /\ portOpen[p] => queue[p] < QueueBound
  /\ IF portOpen[p]
       THEN /\ queue' = [queue EXCEPT ![p] = @ + 1]
            /\ step' = Step("Send", <<c, p>>, "ok")
       ELSE /\ UNCHANGED queue
            /\ step' = Step("Send", <<c, p>>, "Ractor::ClosedError")
  /\ UNCHANGED <<aspect, workerAlive, portOpen>>

(* Only the creator receives.  A closed port still hands out its queued    *)
(* messages; once drained it raises.                                       *)
ReceiveOutcome(c, p) ==
  IF Owner[p] /= c THEN "Ractor::Error:creator"
  ELSE IF queue[p] > 0 THEN "ok"
  ELSE IF ~portOpen[p] THEN "Ractor::ClosedError"
  ELSE "nil"

TakeIfOk(outcome, p) ==
  IF outcome = "ok" THEN queue' = [queue EXCEPT ![p] = @ - 1] ELSE UNCHANGED queue

ReceiveValue(c, p) == IF ReceiveOutcome(c, p) = "ok" THEN "msg" ELSE ""

(* Ractor::Port#receive / Ractor.receive without a timeout.  It blocks on   *)
(* an open empty port, so the model enables it only when it returns.       *)
Receive(c, p) ==
  /\ Alive(c)
  /\ ReceiveOutcome(c, p) /= "nil"
  /\ TakeIfOk(ReceiveOutcome(c, p), p)
  /\ step' = StepV("Receive", <<c, p>>, ReceiveOutcome(c, p), ReceiveValue(c, p))
  /\ UNCHANGED <<aspect, workerAlive, portOpen>>

(* receive(timeout: 0): the same rules, and nil instead of blocking.       *)
Poll(c, p) ==
  /\ Alive(c)
  /\ TakeIfOk(ReceiveOutcome(c, p), p)
  /\ step' = StepV("Poll", <<c, p>>, ReceiveOutcome(c, p), ReceiveValue(c, p))
  /\ UNCHANGED <<aspect, workerAlive, portOpen>>

(* Ractor::Port#close / Ractor#close: creator only, idempotent, keeps the  *)
(* queued messages.                                                        *)
Close(c, p) ==
  /\ Alive(c)
  /\ IF Owner[p] /= c
       THEN /\ UNCHANGED portOpen
            /\ step' = Step("Close", <<c, p>>, "Ractor::Error:closer")
       ELSE /\ portOpen' = [portOpen EXCEPT ![p] = FALSE]
            /\ step' = Step("Close", <<c, p>>, "ok")
  /\ UNCHANGED <<aspect, workerAlive, queue>>

(* Ractor.select over ports.  A port of another Ractor is rejected first.   *)
(* Then a closed, drained port raises even when another port is ready.     *)
(* A closed port with queued messages counts as ready.                      *)
SelectSet(a) == {a[2], a[3]}
Ready(S) == {p \in S : queue[p] > 0}

SelectOutcome(a) ==
  IF \E p \in SelectSet(a) : Owner[p] /= a[1] THEN "Ractor::Error:creator"
  ELSE IF \E p \in SelectSet(a) : ~portOpen[p] /\ queue[p] = 0 THEN "Ractor::ClosedError"
  ELSE IF Ready(SelectSet(a)) /= {} THEN "ok"
  ELSE "nil"

(* With two ready ports the chosen one is not specified, so the model does *)
(* not select then.                                                        *)
SelectEnabled(a) ==
  /\ Alive(a[1])
  /\ SelectOutcome(a) = "ok" => Cardinality(Ready(SelectSet(a))) = 1

Chosen(a) == CHOOSE p \in Ready(SelectSet(a)) : TRUE

SelectTake(a) ==
  IF SelectOutcome(a) = "ok"
    THEN queue' = [queue EXCEPT ![Chosen(a)] = @ - 1]
    ELSE UNCHANGED queue

(* An ok select returns the delivering port and its message.               *)
SelectValue(a) == IF SelectOutcome(a) = "ok" THEN PortName[Chosen(a)] ELSE ""

Select(a) ==
  /\ SelectEnabled(a)
  /\ SelectOutcome(a) /= "nil"
  /\ SelectTake(a)
  /\ step' = StepV("Select", a, SelectOutcome(a), SelectValue(a))
  /\ UNCHANGED <<aspect, workerAlive, portOpen>>

SelectPoll(a) ==
  /\ SelectEnabled(a)
  /\ SelectTake(a)
  /\ step' = StepV("SelectPoll", a, SelectOutcome(a), SelectValue(a))
  /\ UNCHANGED <<aspect, workerAlive, portOpen>>

(* The worker returns from its block.  Its ports close; their queued       *)
(* messages become unreachable because only the creator could receive.     *)
FinishWorker ==
  /\ workerAlive
  /\ workerAlive' = FALSE
  /\ portOpen' = [p \in Ports |-> IF Owner[p] = Worker THEN FALSE ELSE portOpen[p]]
  /\ step' = Step("FinishWorker", <<>>, "ok")
  /\ UNCHANGED <<aspect, queue>>

Next ==
  \/ \E c \in Ractors, p \in Ports : Send(c, p)
  \/ \E c \in Ractors, p \in Ports : Receive(c, p)
  \/ \E c \in Ractors, p \in Ports : Poll(c, p)
  \/ \E c \in Ractors, p \in Ports : Close(c, p)
  \/ \E a \in SelectArgs : Select(a)
  \/ \E a \in SelectArgs : SelectPoll(a)
  \/ FinishWorker

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
  /\ aspect = "ports"
  /\ workerAlive \in BOOLEAN
  /\ portOpen \in [Ports -> BOOLEAN]
  /\ queue \in [Ports -> 0..QueueBound]
  /\ step.op \in Ops
  /\ step.outcome \in Outcomes
  /\ step.args \in Seq(1..3)
  /\ step.value \in Values

(* A terminated Ractor has no open port.                                   *)
TerminatedWorkerPortsClosed ==
  ~workerAlive => \A p \in Ports : Owner[p] = Worker => ~portOpen[p]

(* An ok receive or select always came from the creator of every port it   *)
(* names.                                                                   *)
OnlyCreatorReceives ==
  step.outcome = "ok" =>
    /\ step.op \in {"Receive", "Poll"} => Owner[step.args[2]] = step.args[1]
    /\ step.op \in {"Select", "SelectPoll"} =>
         \A i \in 2..Len(step.args) : Owner[step.args[i]] = step.args[1]

(* A call that raises leaves every port and queue as it was.               *)
ErrorsChangeNothing ==
  [][step'.outcome \notin {"ok", "nil"} => UNCHANGED <<workerAlive, portOpen, queue>>]_vars

(* A closed port never reopens.                                            *)
ClosedStaysClosed ==
  [][\A p \in Ports : ~portOpen[p] => ~portOpen'[p]]_vars

=============================================================================
