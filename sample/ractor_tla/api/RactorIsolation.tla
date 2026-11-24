--------------------------- MODULE RactorIsolation --------------------------
(***************************************************************************)
(* Valid and invalid access across Ractors: class and module ownership,   *)
(* constants, instance variables and class variables of classes, global    *)
(* variables, main-only methods, finalizers, shareable procs, block        *)
(* isolation, Class#dup, and classes whose owner has terminated.           *)
(*                                                                         *)
(* Main (1) owns class K (1).  The worker (2) creates class L (2) and      *)
(* hands it to main.  One aspect is chosen in Init, so TLC explores each   *)
(* section of the design document as its own subgraph:                     *)
(*   "const" "ivar" "cvar": one slot per class, written and read by both.  *)
(*   "method": define, remove, and alias a method.                         *)
(*   "include": include a module of the caller into a class.               *)
(*   "freeze": Module#freeze.                                              *)
(*   "global": $gv and the Ractor-local $stderr.                           *)
(*   "mainonly": an unsafe C method, at_exit, and Regexp.timeout=.          *)
(*   "finalizer": ObjectSpace.define_finalizer on own and foreign objects. *)
(*   "proc": Ractor.shareable_proc with each kind of capture.              *)
(*   "block": Ractor.new with each kind of capture.                        *)
(*   "dup": Class#dup of an own and of a foreign class.                    *)
(*   "readonly": class L after its owner terminated.                       *)
(*                                                                         *)
(* Integer codes in `step.args`: kinds 1 shareable, 2 unshareable;         *)
(* captures 1 none, 2 shareable, 3 unshareable, 4 reassigned,               *)
(* 5 shareable self, 6 unshareable self; finalizer targets 1 K, 2 L,        *)
(* 3 an object of the caller.                                              *)
(***************************************************************************)
EXTENDS Naturals, Sequences

Main == 1
Worker == 2
Ractors == {Main, Worker}

K == 1
L == 2
Classes == {K, L}
None == 0

Aspects == {"const", "ivar", "cvar", "method", "include", "freeze", "global", "mainonly",
            "finalizer", "proc", "block", "dup", "readonly"}
SlotAspects == {"const", "ivar", "cvar", "readonly"}
Slots == {"unset", "shareable", "unshareable"}
Shareable == 1
Unshareable == 2
Kinds == {Shareable, Unshareable}
ProcCaptures == 1..6
BlockCaptures == 1..4
OwnObject == 3
Targets == {K, L, OwnObject}

Outcomes == {"ok", "nil", "NameError", "FrozenError", "Ractor::IsolationError", "Ractor::UnsafeError"}
Ops == {"Init", "Set", "Get", "DefineMethod", "RemoveMethod", "AliasMethod", "Include",
        "FreezeClass", "GlobalGet", "GlobalSet", "StderrGet", "CallUnsafe", "AtExit",
        "RegexpTimeout", "DefineFinalizer", "ShareableProc", "NewRactor", "DupClass",
        "Instantiate", "FinishWorker"}

VARIABLES
  aspect,
  alive,       \* the worker, owner of L, has not terminated
  slot,        \* the modeled slot of each class: unset, shareable, or unshareable
  slotSet,     \* slot /= "unset", what const_defined? and friends answer
  defined,     \* the modeled method exists in the class
  included,    \* the caller's module was included into the class
  frozenClass, \* Module#freeze was applied
  step

vars == <<aspect, alive, slot, slotSet, defined, included, frozenClass, step>>

Step(op, args, outcome) == [op |-> op, args |-> args, outcome |-> outcome]

Init ==
  /\ aspect \in Aspects
  /\ alive = TRUE
  /\ slot = [t \in Classes |-> "unset"]
  /\ slotSet = [t \in Classes |-> FALSE]
  /\ defined = [t \in Classes |-> FALSE]
  /\ included = [t \in Classes |-> FALSE]
  /\ frozenClass = [t \in Classes |-> FALSE]
  /\ step = Step("Init", <<>>, "ok")

Active(c) == c = Main \/ alive

(* The owner of a class is the Ractor that created it, as long as that    *)
(* Ractor lives.  After the owner terminated the class has no owner, and   *)
(* nobody may write it.                                                    *)
OwnerOf(t) == IF t = K THEN Main ELSE IF alive THEN Worker ELSE None
IsOwner(c, t) == OwnerOf(t) = c

Unchanged == UNCHANGED <<aspect, alive, slot, slotSet, defined, included, frozenClass>>

IsolationError == "Ractor::IsolationError"

-----------------------------------------------------------------------------
(* Constants, instance variables, class variables of a class: only the     *)
(* owner writes; anyone reads a shareable value; only the owner reads an    *)
(* unshareable value.  The readonly aspect uses the constant slot of L.    *)
(* CRuby checks ownership and frozenness in an order that depends on the  *)
(* operation: constants and class variables test ownership first;         *)
(* instance variables, methods, and include test frozenness first.         *)
SlotEnabled(t) == aspect \in SlotAspects /\ (aspect = "readonly" => t = L)

OwnerFirst(c, t) ==
  IF ~IsOwner(c, t) THEN IsolationError
  ELSE IF frozenClass[t] THEN "FrozenError"
  ELSE "ok"

FrozenFirst(c, t) ==
  IF frozenClass[t] THEN "FrozenError"
  ELSE IF ~IsOwner(c, t) THEN IsolationError
  ELSE "ok"

WriteOutcome(c, t) == IF aspect = "ivar" THEN FrozenFirst(c, t) ELSE OwnerFirst(c, t)

Set(c, t, k) ==
  /\ SlotEnabled(t)
  /\ Active(c)
  /\ IF WriteOutcome(c, t) = "ok"
       THEN /\ slot' = [slot EXCEPT ![t] = IF k = Shareable THEN "shareable" ELSE "unshareable"]
            /\ slotSet' = [slotSet EXCEPT ![t] = TRUE]
            /\ step' = Step("Set", <<c, t, k>>, "ok")
       ELSE /\ UNCHANGED <<slot, slotSet>>
            /\ step' = Step("Set", <<c, t, k>>, WriteOutcome(c, t))
  /\ UNCHANGED <<aspect, alive, defined, included, frozenClass>>

GetOutcome(c, t) ==
  IF slot[t] = "unset" THEN (IF aspect = "ivar" THEN "nil" ELSE "NameError")
  ELSE IF slot[t] = "shareable" THEN "ok"
  ELSE IF IsOwner(c, t) THEN "ok"
  ELSE IsolationError

Get(c, t) ==
  /\ SlotEnabled(t)
  /\ Active(c)
  /\ step' = Step("Get", <<c, t>>, GetOutcome(c, t))
  /\ Unchanged

(* Methods: define, remove, alias.  rb_class_modify_check tests frozenness, *)
(* then ownership, so a non-owner never sees NameError.                     *)
DefineMethod(c, t) ==
  /\ aspect \in {"method", "readonly"}
  /\ aspect = "readonly" => t = L
  /\ Active(c)
  /\ IF FrozenFirst(c, t) = "ok"
       THEN /\ defined' = [defined EXCEPT ![t] = TRUE]
            /\ step' = Step("DefineMethod", <<c, t>>, "ok")
       ELSE /\ UNCHANGED defined
            /\ step' = Step("DefineMethod", <<c, t>>, FrozenFirst(c, t))
  /\ UNCHANGED <<aspect, alive, slot, slotSet, included, frozenClass>>

RemoveMethod(c, t) ==
  /\ aspect = "method"
  /\ Active(c)
  /\ IF FrozenFirst(c, t) /= "ok"
       THEN /\ UNCHANGED defined
            /\ step' = Step("RemoveMethod", <<c, t>>, FrozenFirst(c, t))
       ELSE IF defined[t]
         THEN /\ defined' = [defined EXCEPT ![t] = FALSE]
              /\ step' = Step("RemoveMethod", <<c, t>>, "ok")
         ELSE /\ UNCHANGED defined
              /\ step' = Step("RemoveMethod", <<c, t>>, "NameError")
  /\ UNCHANGED <<aspect, alive, slot, slotSet, included, frozenClass>>

AliasMethod(c, t) ==
  /\ aspect = "method"
  /\ Active(c)
  /\ step' = Step("AliasMethod", <<c, t>>,
                  IF FrozenFirst(c, t) /= "ok" THEN FrozenFirst(c, t)
                  ELSE IF defined[t] THEN "ok" ELSE "NameError")
  /\ Unchanged

(* Module#include of a module the caller created.                          *)
Include(c, t) ==
  /\ aspect = "include"
  /\ Active(c)
  /\ IF FrozenFirst(c, t) = "ok"
       THEN /\ included' = [included EXCEPT ![t] = TRUE]
            /\ step' = Step("Include", <<c, t>>, "ok")
       ELSE /\ UNCHANGED included
            /\ step' = Step("Include", <<c, t>>, FrozenFirst(c, t))
  /\ UNCHANGED <<aspect, alive, slot, slotSet, defined, frozenClass>>

(* Module#freeze is a modification, so the owner alone may freeze a class. *)
(* Freezing a class that is frozen already changes nothing and is allowed  *)
(* from any Ractor.                                                        *)
FreezeClass(c, t) ==
  /\ aspect \in {"freeze", "readonly"}
  /\ aspect = "readonly" => t = L
  /\ Active(c)
  /\ IF IsOwner(c, t) \/ frozenClass[t]
       THEN /\ frozenClass' = [frozenClass EXCEPT ![t] = TRUE]
            /\ step' = Step("FreezeClass", <<c, t>>, "ok")
       ELSE /\ UNCHANGED frozenClass
            /\ step' = Step("FreezeClass", <<c, t>>, IsolationError)
  /\ UNCHANGED <<aspect, alive, slot, slotSet, defined, included>>

(* Global variables belong to main; $stderr is Ractor-local.               *)
MainOnlyOutcome(c) == IF c = Main THEN "ok" ELSE IsolationError

GlobalGet(c) ==
  /\ aspect = "global"
  /\ Active(c)
  /\ step' = Step("GlobalGet", <<c>>, MainOnlyOutcome(c))
  /\ Unchanged

GlobalSet(c) ==
  /\ aspect = "global"
  /\ Active(c)
  /\ step' = Step("GlobalSet", <<c>>, MainOnlyOutcome(c))
  /\ Unchanged

StderrGet(c) ==
  /\ aspect = "global"
  /\ Active(c)
  /\ step' = Step("StderrGet", <<c>>, "ok")
  /\ Unchanged

(* A C method of an extension that is not declared Ractor-safe raises      *)
(* Ractor::UnsafeError outside main.  at_exit and Regexp.timeout= are      *)
(* main-only and raise Ractor::IsolationError.                             *)
CallUnsafe(c) ==
  /\ aspect = "mainonly"
  /\ Active(c)
  /\ step' = Step("CallUnsafe", <<c>>, IF c = Main THEN "ok" ELSE "Ractor::UnsafeError")
  /\ Unchanged

AtExit(c) ==
  /\ aspect = "mainonly"
  /\ Active(c)
  /\ step' = Step("AtExit", <<c>>, MainOnlyOutcome(c))
  /\ Unchanged

RegexpTimeout(c) ==
  /\ aspect = "mainonly"
  /\ Active(c)
  /\ step' = Step("RegexpTimeout", <<c>>, IF c = Main THEN "nil" ELSE IsolationError)
  /\ Unchanged

(* A finalizer belongs to the object's Ractor: own objects and own classes *)
(* accept it, a class of another Ractor refuses.                           *)
DefineFinalizer(c, target) ==
  /\ aspect = "finalizer"
  /\ Active(c)
  /\ step' = Step("DefineFinalizer", <<c, target>>,
                  IF target = OwnObject \/ IsOwner(c, target) THEN "ok" ELSE IsolationError)
  /\ Unchanged

(* Ractor.shareable_proc: the block may only read outer variables that are *)
(* never reassigned and refer to shareable objects; self must be shareable. *)
CaptureOutcome(capture) ==
  IF capture \in {1, 2, 5} THEN "ok" ELSE IsolationError

ShareableProc(c, capture) ==
  /\ aspect = "proc"
  /\ Active(c)
  /\ step' = Step("ShareableProc", <<c, capture>>, CaptureOutcome(capture))
  /\ Unchanged

(* Ractor.new isolates its block by the same capture rules.                *)
NewRactor(c, capture) ==
  /\ aspect = "block"
  /\ Active(c)
  /\ step' = Step("NewRactor", <<c, capture>>, CaptureOutcome(capture))
  /\ Unchanged

(* Class#dup copies an own class and refuses a class of another Ractor.    *)
DupClass(c, t) ==
  /\ aspect = "dup"
  /\ Active(c)
  /\ step' = Step("DupClass", <<c, t>>, IF IsOwner(c, t) THEN "ok" ELSE IsolationError)
  /\ Unchanged

(* Reading and instantiating a class is allowed from any Ractor.           *)
Instantiate(c, t) ==
  /\ aspect = "readonly"
  /\ t = L
  /\ Active(c)
  /\ step' = Step("Instantiate", <<c, t>>, "ok")
  /\ Unchanged

(* The worker returns; L keeps its contents and loses its owner.           *)
FinishWorker ==
  /\ aspect = "readonly"
  /\ alive
  /\ alive' = FALSE
  /\ step' = Step("FinishWorker", <<>>, "ok")
  /\ UNCHANGED <<aspect, slot, slotSet, defined, included, frozenClass>>

Next ==
  \/ \E c \in Ractors, t \in Classes, k \in Kinds : Set(c, t, k)
  \/ \E c \in Ractors, t \in Classes : Get(c, t)
  \/ \E c \in Ractors, t \in Classes : DefineMethod(c, t)
  \/ \E c \in Ractors, t \in Classes : RemoveMethod(c, t)
  \/ \E c \in Ractors, t \in Classes : AliasMethod(c, t)
  \/ \E c \in Ractors, t \in Classes : Include(c, t)
  \/ \E c \in Ractors, t \in Classes : FreezeClass(c, t)
  \/ \E c \in Ractors : GlobalGet(c)
  \/ \E c \in Ractors : GlobalSet(c)
  \/ \E c \in Ractors : StderrGet(c)
  \/ \E c \in Ractors : CallUnsafe(c)
  \/ \E c \in Ractors : AtExit(c)
  \/ \E c \in Ractors : RegexpTimeout(c)
  \/ \E c \in Ractors, target \in Targets : DefineFinalizer(c, target)
  \/ \E c \in Ractors, capture \in ProcCaptures : ShareableProc(c, capture)
  \/ \E c \in Ractors, capture \in BlockCaptures : NewRactor(c, capture)
  \/ \E c \in Ractors, t \in Classes : DupClass(c, t)
  \/ \E c \in Ractors, t \in Classes : Instantiate(c, t)
  \/ FinishWorker

Spec == Init /\ [][Next]_vars

-----------------------------------------------------------------------------
(* Invariants *)

TypeOK ==
  /\ aspect \in Aspects
  /\ alive \in BOOLEAN
  /\ slot \in [Classes -> Slots]
  /\ slotSet = [t \in Classes |-> slot[t] /= "unset"]
  /\ defined \in [Classes -> BOOLEAN]
  /\ included \in [Classes -> BOOLEAN]
  /\ frozenClass \in [Classes -> BOOLEAN]
  /\ step.op \in Ops
  /\ step.outcome \in Outcomes
  /\ step.args \in Seq(1..6)

(* A class changes only through its owner.                                 *)
OwnerOnlyWrites ==
  [][\/ UNCHANGED <<slot, defined, included, frozenClass>>
     \/ \E t \in Classes : /\ IsOwner(step'.args[1], t)
                           /\ \A u \in Classes \ {t} :
                                slot'[u] = slot[u] /\ defined'[u] = defined[u] /\
                                included'[u] = included[u] /\ frozenClass'[u] = frozenClass[u]]_vars

(* An unshareable slot value is read only by the owner.                    *)
ForeignUnshareableNeverRead ==
  step.op = "Get" /\ step.outcome = "ok" /\ slot[step.args[2]] = "unshareable" => IsOwner(step.args[1], step.args[2])

(* Nothing in L changes after its owner terminated.                        *)
ReadOnlyAfterOwnerExit ==
  [][~alive => slot'[L] = slot[L] /\ defined'[L] = defined[L] /\ frozenClass'[L] = frozenClass[L]]_vars

=============================================================================
