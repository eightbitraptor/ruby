--------------------------- MODULE RactorTransfer ---------------------------
(***************************************************************************)
(* Valid and invalid transfer of objects between Ractors: shareable        *)
(* references, deep copies, moves, Ractor::MovedObject, freeze,            *)
(* Ractor.make_shareable, and objects that refuse to travel.               *)
(*                                                                         *)
(* Main owns a root object (1) and, in most aspects, a leaf object (2) the *)
(* root refers to.  The aspect fixes the shape:                            *)
(*   "string": the root is a mutable String; there is no leaf.             *)
(*   "pair":   the root is an Array holding the leaf twice.                *)
(*   "cycle":  the root is an Array holding the leaf and itself.           *)
(*   "frozen": the root is a frozen Array holding the mutable leaf.        *)
(*   "thread": the root is a Thread; there is no leaf.                     *)
(* The worker receives through its default port.  For each tag it reports *)
(* how it holds the object: "none"; "shared", the sender's object itself;  *)
(* or "mutable" / "frozen", a distinct object with that frozen flag.       *)
(* Copies and moves keep aliases and cycles; the harness reports a lost   *)
(* alias or cycle as a value the model never produces.                    *)
(***************************************************************************)
EXTENDS Naturals, Sequences

Main == 1
Worker == 2

Root == 1
Leaf == 2
Objs == {Root, Leaf}

Aspects == {"string", "pair", "cycle", "frozen", "thread"}
States == {"absent", "mutable", "frozen", "moved"}
Held == {"none", "shared", "mutable", "frozen"}
Outcomes == {"ok", "nil", "Ractor::Error", "Ractor::MovedError"}
Ops == {"Init", "Freeze", "MakeShareable", "MakeShareableCopy", "SendCopy", "SendMove",
        "Receive", "Touch", "NewRactorArg"}

VARIABLES
  aspect,
  mainState, \* main's view of each object: absent, mutable, frozen, or moved
  shareable, \* what Ractor.shareable? answers main for each object
  inbox,     \* how each object waits in the worker's default port, or "none"
  workerHas, \* how the worker holds each object, or "none"
  argRactor, \* an extra Ractor was created with an object argument
  step

vars == <<aspect, mainState, shareable, inbox, workerHas, argRactor, step>>

Step(op, args, outcome) == [op |-> op, args |-> args, outcome |-> outcome]

HasLeaf == aspect \in {"pair", "cycle", "frozen"}

(* Objects reachable from o, including o. *)
Reach(o) == IF o = Root /\ HasLeaf THEN {Root, Leaf} ELSE {o}

(* Ractor.shareable? as CRuby answers it: a moved shell counts as          *)
(* shareable, a Thread never does, everything else must be frozen all the *)
(* way down.                                                              *)
ShareableIn(ms, o) ==
  /\ ms[o] /= "absent"
  /\ \/ ms[o] = "moved"
     \/ /\ aspect /= "thread"
        /\ \A x \in Reach(o) : ms[x] \in {"frozen", "moved"}

Present(o) == mainState[o] /= "absent"
Intact(o) == \A x \in Reach(o) : mainState[x] \in {"mutable", "frozen"}

Nothing == [o \in Objs |-> "none"]
Empty == inbox = Nothing

(* What the worker gets for each object reachable from o: a shareable     *)
(* object itself, any other object as a distinct one with its frozen flag. *)
Delivery(o) == [x \in Objs |->
  IF x \notin Reach(o) THEN "none"
  ELSE IF shareable[x] THEN "shared"
  ELSE mainState[x]]

Init ==
  /\ aspect \in Aspects
  /\ mainState = [o \in Objs |->
       IF o = Root THEN (IF aspect = "frozen" THEN "frozen" ELSE "mutable")
       ELSE IF aspect \in {"pair", "cycle", "frozen"} THEN "mutable" ELSE "absent"]
  /\ shareable = [o \in Objs |-> ShareableIn(mainState, o)]
  /\ inbox = Nothing
  /\ workerHas = Nothing
  /\ argRactor = FALSE
  /\ step = Step("Init", <<>>, "ok")

UpdateShareable == shareable' = [o \in Objs |-> ShareableIn(mainState', o)]

-----------------------------------------------------------------------------
(* obj.freeze by main: shallow.  A moved shell answers no method.          *)
Freeze(o) ==
  /\ Present(o)
  /\ IF mainState[o] = "moved"
       THEN /\ UNCHANGED mainState
            /\ step' = Step("Freeze", <<o>>, "Ractor::MovedError")
       ELSE /\ mainState' = [mainState EXCEPT ![o] = "frozen"]
            /\ step' = Step("Freeze", <<o>>, "ok")
  /\ UpdateShareable
  /\ UNCHANGED <<aspect, inbox, workerHas, argRactor>>

(* Ractor.make_shareable(obj): freezes every reachable object.  A Thread    *)
(* cannot be made shareable.  Graphs with a moved object are not modeled.  *)
MakeShareable(o) ==
  /\ Present(o)
  /\ Intact(o)
  /\ IF aspect = "thread"
       THEN /\ UNCHANGED mainState
            /\ step' = Step("MakeShareable", <<o>>, "Ractor::Error")
       ELSE /\ mainState' = [x \in Objs |-> IF x \in Reach(o) THEN "frozen" ELSE mainState[x]]
            /\ step' = Step("MakeShareable", <<o>>, "ok")
  /\ UpdateShareable
  /\ UNCHANGED <<aspect, inbox, workerHas, argRactor>>

(* Ractor.make_shareable(obj, copy: true): a shareable deep copy; the      *)
(* original and its graph are untouched.                                   *)
MakeShareableCopy(o) ==
  /\ Present(o)
  /\ Intact(o)
  /\ step' = Step("MakeShareableCopy", <<o>>, IF aspect = "thread" THEN "Ractor::Error" ELSE "ok")
  /\ UNCHANGED <<aspect, mainState, shareable, inbox, workerHas, argRactor>>

(* worker.send(obj): a shareable object travels by reference, anything    *)
(* else as a deep copy with its aliases and cycles.  A Thread refuses.     *)
SendCopy(o) ==
  /\ Present(o)
  /\ Intact(o)
  /\ Empty
  /\ IF aspect = "thread"
       THEN /\ UNCHANGED inbox
            /\ step' = Step("SendCopy", <<o>>, "Ractor::Error")
       ELSE /\ inbox' = Delivery(o)
            /\ step' = Step("SendCopy", <<o>>, "ok")
  /\ UNCHANGED <<aspect, mainState, shareable, workerHas, argRactor>>

(* worker.send(obj, move: true): every unshareable object reachable from  *)
(* obj becomes a Ractor::MovedObject in main; shareable ones are shared.   *)
SendMove(o) ==
  /\ Present(o)
  /\ Intact(o)
  /\ Empty
  /\ IF aspect = "thread"
       THEN /\ UNCHANGED <<inbox, mainState>>
            /\ step' = Step("SendMove", <<o>>, "Ractor::Error")
       ELSE /\ inbox' = Delivery(o)
            /\ mainState' = [x \in Objs |->
                 IF x \in Reach(o) /\ ~shareable[x] THEN "moved" ELSE mainState[x]]
            /\ step' = Step("SendMove", <<o>>, "ok")
  /\ UpdateShareable
  /\ UNCHANGED <<aspect, workerHas, argRactor>>

(* Ractor.receive(timeout: 0) in the worker: it now holds the whole graph. *)
Receive ==
  /\ IF Empty
       THEN /\ UNCHANGED workerHas
            /\ step' = Step("Receive", <<>>, "nil")
       ELSE /\ workerHas' = [x \in Objs |-> IF inbox[x] /= "none" THEN inbox[x] ELSE workerHas[x]]
            /\ step' = Step("Receive", <<>>, "ok")
  /\ inbox' = Nothing
  /\ UNCHANGED <<aspect, mainState, shareable, argRactor>>

(* A method call on the object: main's moved shells raise, everything the *)
(* worker holds works.                                                     *)
Touch(c, o) ==
  /\ IF c = Main
       THEN /\ Present(o)
            /\ step' = Step("Touch", <<c, o>>,
                            IF mainState[o] = "moved" THEN "Ractor::MovedError" ELSE "ok")
       ELSE /\ workerHas[o] /= "none"
            /\ step' = Step("Touch", <<c, o>>, "ok")
  /\ UNCHANGED <<aspect, mainState, shareable, inbox, workerHas, argRactor>>

(* Ractor.new(obj) { } passes the argument like worker.send(obj).          *)
NewRactorArg(o) ==
  /\ Present(o)
  /\ Intact(o)
  /\ ~argRactor
  /\ argRactor' = TRUE
  /\ step' = Step("NewRactorArg", <<o>>, IF aspect = "thread" THEN "Ractor::Error" ELSE "ok")
  /\ UNCHANGED <<aspect, mainState, shareable, inbox, workerHas>>

Next ==
  \/ \E o \in Objs : Freeze(o)
  \/ \E o \in Objs : MakeShareable(o)
  \/ \E o \in Objs : MakeShareableCopy(o)
  \/ \E o \in Objs : SendCopy(o)
  \/ \E o \in Objs : SendMove(o)
  \/ Receive
  \/ \E c \in {Main, Worker}, o \in Objs : Touch(c, o)
  \/ \E o \in Objs : NewRactorArg(o)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
  /\ aspect \in Aspects
  /\ mainState \in [Objs -> States]
  /\ shareable \in [Objs -> BOOLEAN]
  /\ inbox \in [Objs -> Held]
  /\ workerHas \in [Objs -> Held]
  /\ argRactor \in BOOLEAN
  /\ step.op \in Ops
  /\ step.outcome \in Outcomes
  /\ step.args \in Seq(1..2)

(* A moved object is never lost: it waits in the worker's inbox or the     *)
(* worker holds it.                                                        *)
MovedIsHeld ==
  \A o \in Objs : mainState[o] = "moved" => workerHas[o] /= "none" \/ inbox[o] /= "none"

(* A moved object stays moved; a frozen object stays frozen.               *)
Monotone ==
  [][\A o \in Objs : /\ mainState[o] = "moved" => mainState'[o] = "moved"
                     /\ mainState[o] = "frozen" => mainState'[o] \in {"frozen", "moved"}]_vars

(* A Thread never becomes shareable and never leaves its Ractor.           *)
ThreadStays ==
  aspect = "thread" => ~shareable[Root] /\ workerHas[Root] = "none" /\ mainState[Root] /= "moved"

=============================================================================
