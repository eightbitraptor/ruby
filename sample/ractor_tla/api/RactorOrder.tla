---------------------------- MODULE RactorOrder ----------------------------
(***************************************************************************)
(* Message order and the choice among ready sources.                       *)
(*                                                                         *)
(* fifo: main (1) and two workers (2, 3) send numbered messages to two     *)
(* ports of main.  Each port hands out its messages in the order they      *)
(* were sent, whoever sent them, also after it was closed.  Ractor.select  *)
(* with both ports ready takes the first message of one of them; the       *)
(* model allows either port, in either argument order.                     *)
(*                                                                         *)
(* mixed: Ractor.select over a port and terminated workers.  Any ready     *)
(* source may win.  A worker whose value was already taken still counts as *)
(* ready, and when it wins the call raises.                                *)
(*                                                                         *)
(* Message <<s, n>> is the n-th message that sender s sent; its value is   *)
(* "m<s>.<n>".  Every receive and select here uses `timeout: 0`.           *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets, TLC

Main == 1
W1 == 2
W2 == 3
Workers == {W1, W2}
RactorName == <<"main", "W1", "W2">>

P1 == 1
P2 == 2
Ports == {P1, P2}
PortName == <<"P1", "P2">>

QueueBound == 2
SendBound == 2
TotalBound == 3

Aspects == {"fifo", "mixed"}
Senders(a) == IF a = "fifo" THEN {Main, W1, W2} ELSE {Main}
UsedPorts(a) == IF a = "fifo" THEN Ports ELSE {P1}
(* Every sender uses P1; only W1 also sends to P2, and only P1 is closed.  *)
SendPorts(a, s) == IF a = "fifo" /\ s = W1 THEN Ports ELSE {P1}

(* Ractor.select arguments: a port is <<"port", p>>, a worker <<"ractor", w>>. *)
APort(p) == <<"port", p>>
ARactor(w) == <<"ractor", w>>
SelectArgs(a) ==
  IF a = "fifo"
    THEN {<<APort(P1), APort(P2)>>, <<APort(P2), APort(P1)>>}
    ELSE {<<ARactor(W1), APort(P1)>>, <<APort(P1), ARactor(W1)>>, <<ARactor(W1), ARactor(W2)>>}
AllSelectArgs == SelectArgs("fifo") \cup SelectArgs("mixed")

Outcomes == {"ok", "nil", "Ractor::ClosedError", "Ractor::Error:taken"}
Ops == {"Init", "Send", "Poll", "Close", "Select", "Finish", "Value"}

VARIABLES
  aspect,
  queue,    \* the messages queued on each port, oldest first
  sent,     \* how many messages each sender sent
  portOpen,
  alive,    \* alive[r]: Ractor r has not terminated; main always runs
  taken,    \* taken[r]: main took the value of worker r (mixed)
  step      \* the last call: [op, args, outcome, value]

vars == <<aspect, queue, sent, portOpen, alive, taken, step>>

Step(op, args, outcome, value) == [op |-> op, args |-> args, outcome |-> outcome, value |-> value]
Label(m) == "m" \o ToString(m[1]) \o "." \o ToString(m[2])

Init ==
  /\ aspect \in Aspects
  /\ queue = [p \in Ports |-> <<>>]
  /\ sent = [s \in {Main, W1, W2} |-> 0]
  /\ portOpen = [p \in Ports |-> TRUE]
  /\ alive = [r \in {Main, W1, W2} |-> TRUE]
  /\ taken = [r \in {Main, W1, W2} |-> FALSE]
  /\ step = Step("Init", <<>>, "ok", "")

Ready(p) == queue[p] /= <<>>
Drained(p) == ~portOpen[p] /\ queue[p] = <<>>

-----------------------------------------------------------------------------
(* Port#send: appends to the queue of an open port.                        *)
Send(s, p) ==
  /\ s \in Senders(aspect)
  /\ p \in SendPorts(aspect, s)
  /\ sent[s] < SendBound
  /\ sent[Main] + sent[W1] + sent[W2] < TotalBound
  /\ IF portOpen[p]
       THEN /\ Len(queue[p]) < QueueBound
            /\ queue' = [queue EXCEPT ![p] = Append(@, <<s, sent[s] + 1>>)]
            /\ sent' = [sent EXCEPT ![s] = @ + 1]
            /\ step' = Step("Send", <<s, p>>, "ok", "")
       ELSE /\ step' = Step("Send", <<s, p>>, "Ractor::ClosedError", "")
            /\ UNCHANGED <<queue, sent>>
  /\ UNCHANGED <<aspect, portOpen, alive, taken>>

(* receive(timeout: 0) by main: the oldest message, nil, or ClosedError.   *)
Poll(p) ==
  /\ p \in UsedPorts(aspect)
  /\ IF Ready(p)
       THEN /\ queue' = [queue EXCEPT ![p] = Tail(@)]
            /\ step' = Step("Poll", <<p>>, "ok", Label(Head(queue[p])))
       ELSE /\ step' = Step("Poll", <<p>>, IF portOpen[p] THEN "nil" ELSE "Ractor::ClosedError", "")
            /\ UNCHANGED queue
  /\ UNCHANGED <<aspect, sent, portOpen, alive, taken>>

(* Port#close keeps the queued messages in their order.                    *)
Close(p) ==
  /\ p = P1
  /\ portOpen' = [portOpen EXCEPT ![p] = FALSE]
  /\ step' = Step("Close", <<p>>, "ok", "")
  /\ UNCHANGED <<aspect, queue, sent, alive, taken>>

-----------------------------------------------------------------------------
(* Ractor.select(..., timeout: 0).  A closed and drained port raises       *)
(* first.  Then any ready source wins: a port hands out its oldest         *)
(* message, a terminated worker its value, or Ractor::Error when the value *)
(* was taken.  With nothing ready the call returns nil.                    *)
Source(x) == x[2]
PortsIn(args) == {Source(args[i]) : i \in {i \in 1..Len(args) : args[i][1] = "port"}}
WorkersIn(args) == {Source(args[i]) : i \in {i \in 1..Len(args) : args[i][1] = "ractor"}}
ReadyIn(args) == {APort(p) : p \in {q \in PortsIn(args) : Ready(q)}}
                 \cup {ARactor(w) : w \in {v \in WorkersIn(args) : ~alive[v]}}

Select(args) ==
  /\ args \in SelectArgs(aspect)
  /\ IF \E p \in PortsIn(args) : Drained(p)
       THEN /\ step' = Step("Select", args, "Ractor::ClosedError", "")
            /\ UNCHANGED <<queue, taken>>
     ELSE IF ReadyIn(args) = {}
       THEN /\ step' = Step("Select", args, "nil", "")
            /\ UNCHANGED <<queue, taken>>
     ELSE \E x \in ReadyIn(args) :
            IF x[1] = "port"
              THEN /\ queue' = [queue EXCEPT ![x[2]] = Tail(@)]
                   /\ step' = Step("Select", args, "ok", PortName[x[2]] \o " " \o Label(Head(queue[x[2]])))
                   /\ UNCHANGED taken
            ELSE IF taken[x[2]]
              THEN /\ step' = Step("Select", args, "Ractor::Error:taken", "")
                   /\ UNCHANGED <<queue, taken>>
            ELSE /\ taken' = [taken EXCEPT ![x[2]] = TRUE]
                 /\ step' = Step("Select", args, "ok", RactorName[x[2]])
                 /\ UNCHANGED queue
  /\ UNCHANGED <<aspect, sent, portOpen, alive>>

(* The worker returns :finished from its block.                            *)
Finish(w) ==
  /\ aspect = "mixed"
  /\ alive[w]
  /\ alive' = [alive EXCEPT ![w] = FALSE]
  /\ step' = Step("Finish", <<w>>, "ok", "")
  /\ UNCHANGED <<aspect, queue, sent, portOpen, taken>>

(* Ractor#value by main on a terminated worker.                            *)
Value(w) ==
  /\ aspect = "mixed"
  /\ ~alive[w]
  /\ IF taken[w]
       THEN /\ step' = Step("Value", <<w>>, "Ractor::Error:taken", "")
            /\ UNCHANGED taken
       ELSE /\ taken' = [taken EXCEPT ![w] = TRUE]
            /\ step' = Step("Value", <<w>>, "ok", "finished")
  /\ UNCHANGED <<aspect, queue, sent, portOpen, alive>>

Next ==
  \/ \E s \in {Main, W1, W2}, p \in Ports : Send(s, p)
  \/ \E p \in Ports : Poll(p) \/ Close(p)
  \/ \E args \in AllSelectArgs : Select(args)
  \/ \E w \in Workers : Finish(w) \/ Value(w)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Invariants *)

Messages == {<<s, n>> : s \in {Main, W1, W2}, n \in 1..SendBound}

TypeOK ==
  /\ aspect \in Aspects
  /\ \A p \in Ports : queue[p] \in Seq(Messages) /\ Len(queue[p]) <= QueueBound
  /\ sent \in [{Main, W1, W2} -> 0..SendBound]
  /\ portOpen \in [Ports -> BOOLEAN]
  /\ alive \in [{Main, W1, W2} -> BOOLEAN] /\ alive[Main]
  /\ taken \in [{Main, W1, W2} -> BOOLEAN] /\ ~taken[Main]
  /\ step.op \in Ops
  /\ step.outcome \in Outcomes

(* On each port, the messages of one sender keep their send order.         *)
SenderOrderKept ==
  \A p \in Ports : \A i, j \in DOMAIN queue[p] :
    (i < j /\ queue[p][i][1] = queue[p][j][1]) => queue[p][i][2] < queue[p][j][2]

(* No message is queued twice.                                             *)
NoDuplicates ==
  \A p, q \in Ports : \A i \in DOMAIN queue[p], j \in DOMAIN queue[q] :
    (p /= q \/ i /= j) => queue[p][i] /= queue[q][j]

(* Only a terminated worker has its value taken.                           *)
TakenOnlyWhenDone ==
  \A w \in Workers : taken[w] => ~alive[w]

-----------------------------------------------------------------------------
(* Action properties *)

(* A successful receive or select from a port removes exactly its oldest   *)
(* message.                                                                *)
OldestFirst ==
  [][\A p \in Ports : (Len(queue'[p]) < Len(queue[p])) => queue'[p] = Tail(queue[p])]_vars

ClosedStaysClosed ==
  [][\A p \in Ports : ~portOpen[p] => ~portOpen'[p]]_vars

=============================================================================
