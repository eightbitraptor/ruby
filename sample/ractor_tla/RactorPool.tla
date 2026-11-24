---------------------------- MODULE RactorPool ----------------------------
(***************************************************************************)
(* A bounded worker-pool protocol built on Ruby Ractors.                   *)
(*                                                                         *)
(* The main Ractor owns a FIFO of jobs.  Each worker Ractor has a default  *)
(* port that receives jobs and a main-owned result port that returns one   *)
(* result.  The protocol permits at most one outstanding message per       *)
(* modeled port: a worker holds one assigned job and one ready result.     *)
(*                                                                         *)
(* Worker creation and monitor registration happen before Init, so they    *)
(* are replay setup and not part of `history`.  `history` records every    *)
(* action token so TLC keeps each schedule prefix as a distinct state.     *)
(* The DOT dump of the state graph is replayed against CRuby by            *)
(* sample/ractor_tla/verify.rb.                                            *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS WorkerCount, JobCount

ASSUME WorkerCount = 2 /\ JobCount = 3

Workers == 1..WorkerCount
Jobs == 1..JobCount
None == 0

(* Job 1 is fully shareable, job 2 travels as a deep copy, job 3 is moved. *)
Mode(j) == CASE j = 1 -> "share"
             [] j = 2 -> "copy"
             [] j = 3 -> "move"

Phases == {"idle", "queued", "received", "ready", "closing", "observed"}

(* Action tokens: tens digit names the action, ones digit names the worker. *)
DispatchToken(w) == 10 + w
ReceiveToken(w) == 20 + w
ProcessToken(w) == 30 + w
CollectToken(w) == 40 + w
GlobalCollectToken == 50
StopToken(w) == 60 + w
ObserveToken(w) == 70 + w

Tokens == {GlobalCollectToken} \cup
          UNION {{DispatchToken(w), ReceiveToken(w), ProcessToken(w),
                  CollectToken(w), StopToken(w), ObserveToken(w)} : w \in Workers}

(* One dispatch and three steps (receive, process, collect) per job, one  *)
(* global collection, then a stop and an observation per worker.          *)
HistoryBound == JobCount + 3 * JobCount + 1 + 2 * WorkerCount

VARIABLES
  pending,          \* FIFO of jobs the main Ractor has not dispatched yet
  phase,            \* protocol phase of each worker
  assignment,       \* job on each worker's default port or in its hands
  result,           \* job whose result sits on each worker's result port
  completed,        \* jobs whose results main has collected
  processedBy,      \* jobs each worker processed, kept after collection
  mainCanAccess,    \* jobs whose objects main may still touch
  localCollections, \* explicit local collections each worker ran
  globalCollected,  \* main ran its one global collection
  valueTaken,       \* main absorbed each worker's termination value
  done,             \* the second worker has been observed
  history           \* tokens of every action taken so far

vars == <<pending, phase, assignment, result, completed, processedBy,
          mainCanAccess, localCollections, globalCollected, valueTaken,
          done, history>>

Init ==
  /\ pending = [i \in 1..JobCount |-> i]
  /\ phase = [w \in Workers |-> "idle"]
  /\ assignment = [w \in Workers |-> None]
  /\ result = [w \in Workers |-> None]
  /\ completed = {}
  /\ processedBy = [w \in Workers |-> {}]
  /\ mainCanAccess = Jobs
  /\ localCollections = [w \in Workers |-> 0]
  /\ globalCollected = FALSE
  /\ valueTaken = [w \in Workers |-> FALSE]
  /\ done = FALSE
  /\ history = <<>>

(* Main dispatches before it does anything else whenever it can.           *)
DispatchPossible == pending /= <<>> /\ \E w \in Workers : phase[w] = "idle"

AllIdle == \A w \in Workers : phase[w] = "idle"

Dispatch(w) ==
  /\ pending /= <<>>
  /\ phase[w] = "idle"
  /\ LET j == Head(pending) IN
       /\ pending' = Tail(pending)
       /\ assignment' = [assignment EXCEPT ![w] = j]
       /\ phase' = [phase EXCEPT ![w] = "queued"]
       /\ mainCanAccess' = IF Mode(j) = "move" THEN mainCanAccess \ {j}
                                               ELSE mainCanAccess
  /\ history' = Append(history, DispatchToken(w))
  /\ UNCHANGED <<result, completed, processedBy, localCollections,
                 globalCollected, valueTaken, done>>

Receive(w) ==
  /\ ~DispatchPossible
  /\ phase[w] = "queued"
  /\ phase' = [phase EXCEPT ![w] = "received"]
  /\ history' = Append(history, ReceiveToken(w))
  /\ UNCHANGED <<pending, assignment, result, completed, processedBy,
                 mainCanAccess, localCollections, globalCollected,
                 valueTaken, done>>

(* A worker publishes its result only while no other result is ready, so   *)
(* every replayed Ractor.select has exactly one ready source.              *)
Process(w) ==
  /\ ~DispatchPossible
  /\ phase[w] = "received"
  /\ \A v \in Workers \ {w} : result[v] = None
  /\ result' = [result EXCEPT ![w] = assignment[w]]
  /\ phase' = [phase EXCEPT ![w] = "ready"]
  /\ processedBy' = [processedBy EXCEPT ![w] = @ \cup {assignment[w]}]
  /\ localCollections' = [localCollections EXCEPT ![w] = @ + 1]
  /\ history' = Append(history, ProcessToken(w))
  /\ UNCHANGED <<pending, assignment, completed, mainCanAccess,
                 globalCollected, valueTaken, done>>

Collect(w) ==
  /\ ~DispatchPossible
  /\ phase[w] = "ready"
  /\ completed' = completed \cup {result[w]}
  /\ result' = [result EXCEPT ![w] = None]
  /\ assignment' = [assignment EXCEPT ![w] = None]
  /\ phase' = [phase EXCEPT ![w] = "idle"]
  /\ history' = Append(history, CollectToken(w))
  /\ UNCHANGED <<pending, processedBy, mainCanAccess, localCollections,
                 globalCollected, valueTaken, done>>

GlobalCollect ==
  /\ completed = Jobs
  /\ AllIdle
  /\ ~globalCollected
  /\ globalCollected' = TRUE
  /\ history' = Append(history, GlobalCollectToken)
  /\ UNCHANGED <<pending, phase, assignment, result, completed, processedBy,
                 mainCanAccess, localCollections, valueTaken, done>>

(* Workers stop in identifier order, each observed before the next stops.  *)
Stop(w) ==
  /\ globalCollected
  /\ phase[w] = "idle"
  /\ \A v \in Workers : v < w => phase[v] = "observed"
  /\ phase' = [phase EXCEPT ![w] = "closing"]
  /\ history' = Append(history, StopToken(w))
  /\ UNCHANGED <<pending, assignment, result, completed, processedBy,
                 mainCanAccess, localCollections, globalCollected,
                 valueTaken, done>>

Observe(w) ==
  /\ phase[w] = "closing"
  /\ phase' = [phase EXCEPT ![w] = "observed"]
  /\ valueTaken' = [valueTaken EXCEPT ![w] = TRUE]
  /\ done' = (\A v \in Workers \ {w} : phase[v] = "observed")
  /\ history' = Append(history, ObserveToken(w))
  /\ UNCHANGED <<pending, assignment, result, completed, processedBy,
                 mainCanAccess, localCollections, globalCollected>>

(* The only stutter action: it keeps the finished run out of TLC's         *)
(* deadlock check while every premature deadlock still fails.              *)
Terminal == done /\ UNCHANGED vars

Next ==
  \/ \E w \in Workers : Dispatch(w)
  \/ \E w \in Workers : Receive(w)
  \/ \E w \in Workers : Process(w)
  \/ \E w \in Workers : Collect(w)
  \/ GlobalCollect
  \/ \E w \in Workers : Stop(w)
  \/ \E w \in Workers : Observe(w)
  \/ Terminal

Spec == Init /\ [][Next]_vars /\ WF_vars(Next)

-----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
  /\ pending \in Seq(Jobs)
  /\ Len(pending) <= JobCount
  /\ phase \in [Workers -> Phases]
  /\ assignment \in [Workers -> Jobs \cup {None}]
  /\ result \in [Workers -> Jobs \cup {None}]
  /\ completed \subseteq Jobs
  /\ processedBy \in [Workers -> SUBSET Jobs]
  /\ mainCanAccess \subseteq Jobs
  /\ localCollections \in [Workers -> 0..JobCount]
  /\ globalCollected \in BOOLEAN
  /\ valueTaken \in [Workers -> BOOLEAN]
  /\ done \in BOOLEAN
  /\ history \in Seq(Tokens)
  /\ Len(history) <= HistoryBound

IsPending(j) == \E i \in DOMAIN pending : pending[i] = j

AssignedTo(j) == {w \in Workers : assignment[w] = j}

(* Every job is pending, assigned to one worker, or completed.            *)
JobConservation ==
  \A j \in Jobs :
    \/ IsPending(j) /\ AssignedTo(j) = {} /\ j \notin completed
    \/ ~IsPending(j) /\ Cardinality(AssignedTo(j)) = 1 /\ j \notin completed
    \/ ~IsPending(j) /\ AssignedTo(j) = {} /\ j \in completed

UniqueAssignment ==
  \A v, w \in Workers :
    (assignment[v] /= None /\ assignment[v] = assignment[w]) => v = w

(* A ready result names the job assigned to that worker and nothing else. *)
ResultIntegrity ==
  \A w \in Workers :
    /\ (result[w] /= None) <=> (phase[w] = "ready")
    /\ result[w] /= None => result[w] = assignment[w]

(* One assigned job per worker and at most one ready result in the pool.  *)
(* The second conjunct restates the guard of Process: it is a restriction *)
(* of the model, not a property TLC derives.                              *)
QueueBound ==
  /\ \A w \in Workers :
       (assignment[w] /= None) <=> (phase[w] \in {"queued", "received", "ready"})
  /\ Cardinality({w \in Workers : result[w] /= None}) <= 1

(* Main keeps access to shared and copied jobs; a moved job leaves main    *)
(* at dispatch and never comes back.                                       *)
TransferOwnership ==
  \A j \in Jobs :
    (j \in mainCanAccess) <=> (Mode(j) /= "move" \/ IsPending(j))

(* One explicit local collection per processed job, and no job processed   *)
(* twice.                                                                  *)
CollectionAccounting ==
  /\ \A w \in Workers : localCollections[w] = Cardinality(processedBy[w])
  /\ \A v, w \in Workers : v /= w => processedBy[v] \cap processedBy[w] = {}
  /\ UNION {processedBy[w] : w \in Workers} =
       completed \cup {result[w] : w \in {v \in Workers : result[v] /= None}}

(* Nothing shuts down before every result is in.                           *)
ShutdownSafety ==
  /\ globalCollected => completed = Jobs /\ pending = <<>>
  /\ \A w \in Workers :
       /\ phase[w] \in {"closing", "observed"} => globalCollected
       /\ valueTaken[w] <=> phase[w] = "observed"
  /\ done <=> \A w \in Workers : valueTaken[w]

-----------------------------------------------------------------------------
(* Liveness under WF_vars(Next) *)

EventuallyCompleted == <>(completed = Jobs)

EventuallyObserved == <>done

=============================================================================
