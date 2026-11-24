------------------------- MODULE RactorTermination -------------------------
(***************************************************************************)
(* Valid and invalid use of Ractor termination: monitor, unmonitor, the    *)
(* exit and abort tokens, join, value, Ractor.select on a Ractor, and the  *)
(* successor rule that lets exactly one Ractor take a termination value.   *)
(*                                                                         *)
(* Ractor 2 terminates once.  The aspect chosen in Init says how: "exit"   *)
(* returns from its block, "abort" raises.  Main (1) and Ractor 3 observe  *)
(* it.  Port 1 belongs to main, port 2 to Ractor 3; each is used as a      *)
(* monitor port by its creator.                                            *)
(*                                                                         *)
(* Ractor::Error outcomes carry the rule from the exception message:       *)
(* `taken` when the successor asks again, `successor` when another Ractor  *)
(* asks after the value was taken.                                         *)
(***************************************************************************)
EXTENDS Naturals, Sequences

Main == 1
Target == 2
Observer == 3
Callers == {Main, Observer}

M == 1
N == 2
Ports == {M, N}
Owner == <<Main, Observer>>

None == 0

(* Bounds of the model: how many times one port may be registered, and how *)
(* many tokens it may hold.  They are not rules of the API.                *)
MonitorBound == 2
TokenBound == 2

Outcomes == {"ok", "nil", "true", "false", "Ractor::RemoteError",
             "Ractor::Error:taken", "Ractor::Error:successor"}
Ops == {"Init", "Monitor", "Unmonitor", "Finish", "ReceiveToken", "Join", "Value", "SelectRactor"}

(* What an ok call returns: the token kind for a received token,           *)
(* "finished" for the value, "selected" for [target, value] from            *)
(* Ractor.select, "self" for the target from join, "" for no stated value.   *)
Values == {"", "exited", "aborted", "finished", "selected", "self"}

VARIABLES
  aspect,    \* "exit" or "abort": how the target terminates
  alive,     \* the target has not terminated
  monitors,  \* registrations of each port on the target, duplicates counted
  tokens,    \* termination tokens queued on each port
  successor, \* the Ractor that took the value or the abort, or None
  step

vars == <<aspect, alive, monitors, tokens, successor, step>>

StepV(op, args, outcome, value) == [op |-> op, args |-> args, outcome |-> outcome, value |-> value]
Step(op, args, outcome) == StepV(op, args, outcome, "")

TokenKind == IF aspect = "exit" THEN "exited" ELSE "aborted"

Init ==
  /\ aspect \in {"exit", "abort"}
  /\ alive = TRUE
  /\ monitors = [p \in Ports |-> 0]
  /\ tokens = [p \in Ports |-> 0]
  /\ successor = None
  /\ step = Step("Init", <<>>, "ok")

-----------------------------------------------------------------------------
(* Ractor#monitor(port) by the port's creator.  While the target runs it     *)
(* registers the port and returns true; every registration yields one       *)
(* token.  After termination it returns false and sends the token at once.  *)
Monitor(c, p) ==
  /\ Owner[p] = c
  /\ IF alive
       THEN /\ monitors[p] < MonitorBound
            /\ monitors' = [monitors EXCEPT ![p] = @ + 1]
            /\ UNCHANGED tokens
            /\ step' = Step("Monitor", <<c, p>>, "true")
       ELSE /\ tokens[p] < TokenBound
            /\ tokens' = [tokens EXCEPT ![p] = @ + 1]
            /\ UNCHANGED monitors
            /\ step' = Step("Monitor", <<c, p>>, "false")
  /\ UNCHANGED <<aspect, alive, successor>>

(* Ractor#unmonitor(port) removes every registration of that port.  It      *)
(* returns the Ractor, also for an unregistered port or after termination.  *)
Unmonitor(c, p) ==
  /\ Owner[p] = c
  /\ monitors' = [monitors EXCEPT ![p] = 0]
  /\ step' = Step("Unmonitor", <<c, p>>, "ok")
  /\ UNCHANGED <<aspect, alive, tokens, successor>>

(* The target terminates.  Each registration sends [target, :exited] or     *)
(* [target, :aborted] to its port, and the registrations are dropped.        *)
Finish ==
  /\ alive
  /\ alive' = FALSE
  /\ tokens' = [p \in Ports |-> tokens[p] + monitors[p]]
  /\ monitors' = [p \in Ports |-> 0]
  /\ step' = Step("Finish", <<>>, "ok")
  /\ UNCHANGED <<aspect, successor>>

(* port.receive(timeout: 0) by the creator: one token, or nil.             *)
ReceiveToken(c, p) ==
  /\ Owner[p] = c
  /\ IF tokens[p] > 0
       THEN /\ tokens' = [tokens EXCEPT ![p] = @ - 1]
            /\ step' = StepV("ReceiveToken", <<c, p>>, "ok", TokenKind)
       ELSE /\ UNCHANGED tokens
            /\ step' = Step("ReceiveToken", <<c, p>>, "nil")
  /\ UNCHANGED <<aspect, alive, monitors, successor>>

(* The first Ractor to take the termination value becomes the successor.    *)
(* An exit hands out the value once; an abort raises Ractor::RemoteError    *)
(* once.  The successor's next call reports the value as taken; every       *)
(* other Ractor is told that only the successor may take it.               *)
TakeOutcome(c) ==
  IF successor = None THEN (IF aspect = "exit" THEN "ok" ELSE "Ractor::RemoteError")
  ELSE IF successor = c THEN "Ractor::Error:taken"
  ELSE "Ractor::Error:successor"

Take(c) ==
  successor' = IF successor = None THEN c ELSE successor

TakeValue(c, value) == IF TakeOutcome(c) = "ok" THEN value ELSE ""

(* Ractor#value waits for termination, so it is enabled once the target    *)
(* has terminated.                                                         *)
Value(c) ==
  /\ ~alive
  /\ Take(c)
  /\ step' = StepV("Value", <<c>>, TakeOutcome(c), TakeValue(c, "finished"))
  /\ UNCHANGED <<aspect, alive, monitors, tokens>>

(* Ractor#join returns the Ractor after an exit, whoever calls and however  *)
(* often.  After an abort it takes the exception like Ractor#value.         *)
Join(c) ==
  /\ ~alive
  /\ IF aspect = "exit"
       THEN /\ UNCHANGED successor
            /\ step' = StepV("Join", <<c>>, "ok", "self")
       ELSE /\ Take(c)
            /\ step' = Step("Join", <<c>>, TakeOutcome(c))
  /\ UNCHANGED <<aspect, alive, monitors, tokens>>

(* Ractor.select(target, timeout: 0): nil while the target runs; after      *)
(* termination it takes the value like Ractor#value.                       *)
SelectRactor(c) ==
  /\ IF alive
       THEN /\ UNCHANGED successor
            /\ step' = Step("SelectRactor", <<c>>, "nil")
       ELSE /\ Take(c)
            /\ step' = StepV("SelectRactor", <<c>>, TakeOutcome(c), TakeValue(c, "selected"))
  /\ UNCHANGED <<aspect, alive, monitors, tokens>>

Next ==
  \/ \E c \in Callers, p \in Ports : Monitor(c, p)
  \/ \E c \in Callers, p \in Ports : Unmonitor(c, p)
  \/ Finish
  \/ \E c \in Callers, p \in Ports : ReceiveToken(c, p)
  \/ \E c \in Callers : Value(c)
  \/ \E c \in Callers : Join(c)
  \/ \E c \in Callers : SelectRactor(c)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
  /\ aspect \in {"exit", "abort"}
  /\ alive \in BOOLEAN
  /\ monitors \in [Ports -> 0..MonitorBound]
  /\ tokens \in [Ports -> 0..TokenBound]
  /\ successor \in {None} \cup Callers
  /\ step.op \in Ops
  /\ step.outcome \in Outcomes
  /\ step.args \in Seq(1..3)
  /\ step.value \in Values

(* Nobody takes a value from a running Ractor, and tokens exist only after *)
(* termination.                                                            *)
NothingBeforeTermination ==
  alive => /\ successor = None
           /\ \A p \in Ports : tokens[p] = 0

(* Registrations exist only before termination.                           *)
NoRegistrationsAfterTermination ==
  ~alive => \A p \in Ports : monitors[p] = 0

(* The successor never changes once set.                                   *)
SuccessorStable ==
  [][successor /= None => successor' = successor]_vars

(* The value or the abort is handed out exactly once.                      *)
TakenOnce ==
  [][step'.outcome \in {"Ractor::RemoteError"} \/ (step'.op \in {"Value", "SelectRactor"} /\ step'.outcome = "ok")
       => successor = None /\ successor' = step'.args[1]]_vars

=============================================================================
