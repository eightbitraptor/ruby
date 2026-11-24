----------------------------- MODULE RactorLocal -----------------------------
(***************************************************************************)
(* Ractor-local storage: Ractor.[], Ractor.[]=, Ractor.store_if_absent,    *)
(* and the Ractor#[] / Ractor#[]= forms on another Ractor.                  *)
(*                                                                         *)
(* Main (1) and the worker (2) each own a store with keys 1 and 2.  Values  *)
(* are shareable (1) or unshareable (2); both are allowed.  Reading or      *)
(* writing another Ractor's store raises RuntimeError.                      *)
(***************************************************************************)
EXTENDS Naturals, Sequences

Main == 1
Worker == 2
Ractors == {Main, Worker}
Other(c) == IF c = Main THEN Worker ELSE Main

Keys == {1, 2}
Shareable == 1
Unshareable == 2
Kinds == {Shareable, Unshareable}
Values == {"unset", "shareable", "unshareable"}

Outcomes == {"ok", "nil", "RuntimeError"}
Ops == {"Init", "Store", "Fetch", "StoreIfAbsent", "FetchOther", "StoreOther"}

VARIABLES
  aspect,
  stored, \* stored[r][k]: what Ractor r keeps under key k
  step

vars == <<aspect, stored, step>>

Step(op, args, outcome) == [op |-> op, args |-> args, outcome |-> outcome]
KindName(k) == IF k = Shareable THEN "shareable" ELSE "unshareable"

Init ==
  /\ aspect = "local"
  /\ stored = [r \in Ractors |-> [k \in Keys |-> "unset"]]
  /\ step = Step("Init", <<>>, "ok")

(* Ractor[key] = value: any object, shareable or not.                      *)
Store(c, k, kind) ==
  /\ stored' = [stored EXCEPT ![c][k] = KindName(kind)]
  /\ step' = Step("Store", <<c, k, kind>>, "ok")
  /\ UNCHANGED aspect

(* Ractor[key]: nil for an unset key.                                      *)
Fetch(c, k) ==
  /\ step' = Step("Fetch", <<c, k>>, IF stored[c][k] = "unset" THEN "nil" ELSE "ok")
  /\ UNCHANGED <<aspect, stored>>

(* Ractor.store_if_absent(key) { value }: the first value wins and is      *)
(* returned; a later call returns the stored value unchanged.              *)
StoreIfAbsent(c, k, kind) ==
  /\ stored' = IF stored[c][k] = "unset" THEN [stored EXCEPT ![c][k] = KindName(kind)] ELSE stored
  /\ step' = Step("StoreIfAbsent", <<c, k, kind>>, "ok")
  /\ UNCHANGED aspect

(* other_ractor[key] and other_ractor[key] = value: only the current        *)
(* Ractor reaches its store.                                               *)
FetchOther(c, k) ==
  /\ step' = Step("FetchOther", <<c, k>>, "RuntimeError")
  /\ UNCHANGED <<aspect, stored>>

StoreOther(c, k, kind) ==
  /\ step' = Step("StoreOther", <<c, k, kind>>, "RuntimeError")
  /\ UNCHANGED <<aspect, stored>>

Next ==
  \/ \E c \in Ractors, k \in Keys, kind \in Kinds : Store(c, k, kind)
  \/ \E c \in Ractors, k \in Keys : Fetch(c, k)
  \/ \E c \in Ractors, k \in Keys, kind \in Kinds : StoreIfAbsent(c, k, kind)
  \/ \E c \in Ractors, k \in Keys : FetchOther(c, k)
  \/ \E c \in Ractors, k \in Keys, kind \in Kinds : StoreOther(c, k, kind)

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
  /\ aspect = "local"
  /\ stored \in [Ractors -> [Keys -> Values]]
  /\ step.op \in Ops
  /\ step.outcome \in Outcomes
  /\ step.args \in Seq(1..2)

(* A Ractor's store changes only through that Ractor's own calls.          *)
StorageIsolated ==
  [][\A r \in Ractors : stored'[r] /= stored[r] => step'.args[1] = r]_vars

(* store_if_absent never replaces a value.                                 *)
StoreIfAbsentKeepsFirst ==
  [][step'.op = "StoreIfAbsent" /\ stored[step'.args[1]][step'.args[2]] /= "unset"
       => stored' = stored]_vars

=============================================================================
