------------------------------ MODULE IpcDelete ------------------------------
(***************************************************************************)
(* Phase 1 work item 8 (optional in plan §11): IPC racing a preemptible    *)
(* task destruction.                                                       *)
(*                                                                         *)
(* The host model (model/) runs every operation atomically, one at a       *)
(* time. The kernel won't: 0003 makes destruction preemptible (a cascade   *)
(* is bounded by the tables, not per operation; docs/abi.md, Open 2), and  *)
(* SMP lets other CPUs run while it is in progress. This spec takes the    *)
(* model's semantics and splits one server task's destruction into the     *)
(* steps the kernel would take between preemption points, in the model's   *)
(* order (task.c, joist__space_destroy):                                   *)
(*                                                                         *)
(*   1. mark the task dead;                                                *)
(*   2. destroy its threads (here: the one server thread);                 *)
(*   3. delete its capability slots one at a time.                         *)
(*                                                                         *)
(* Client threads in another task keep calling the server's endpoint, time *)
(* out, and pass capabilities into the server's space, at any point in     *)
(* between. TLC checks every interleaving against:                         *)
(*                                                                         *)
(*   I6  every caller waiting for a reply is named by exactly one live     *)
(*       reply capability (the server thread's, or a reply object's);      *)
(*   I10 nobody is queued on a dead endpoint, and once destruction ends    *)
(*       every client eventually runs again;                               *)
(*   I1  when destruction ends, the space holds nothing.                   *)
(*                                                                         *)
(* RefuseIntoDying selects whether passing a capability into a space that  *)
(* is being destroyed is refused. Without it, a capability can land in a   *)
(* slot the sweep has already passed and is never deleted (I1).            *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Clients,          \* client threads, in one client task
          MaxCalls,         \* calls per client, to keep the state space finite
          NSlots,           \* slots in the server's space
          RefuseIntoDying   \* BOOLEAN: may a dying space receive capabilities?

None == <<>>

\* The server's space starts with its endpoint's RECV capability in slot 1
\* and a reply object in slot 2; the rest are empty.
InitSlots == [i \in 1..NSlots |-> IF i = 1 THEN "E" ELSE IF i = 2 THEN "R" ELSE "empty"]

VARIABLES
    st,       \* client thread state: "run", "sendq" (queued Call), "reply" (waiting)
    calls,    \* calls started per client (the model's call_seq)
    q,        \* the endpoint's queue of callers (sequence)
    srv,      \* server thread: "run", "recvq" (blocked in Recv), "dead"
    sReply,   \* the server thread's reply capability: <<client, call>> or None
    rObj,     \* the reply object's saved reply capability: <<client, call>> or None
    epAlive,  \* the endpoint's RECV capability still exists (0001: else it is dead)
    slots,    \* the server's capability slots
    phase,    \* "alive", "threads" (task marked dead), "sweep", "done"
    sweep     \* next slot the sweep deletes

vars == <<st, calls, q, srv, sReply, rObj, epAlive, slots, phase, sweep>>

TypeOK ==
    /\ st \in [Clients -> {"run", "sendq", "reply"}]
    /\ calls \in [Clients -> 0..MaxCalls]
    /\ srv \in {"run", "recvq", "dead"}
    /\ sReply = None \/ sReply \in Clients \X (1..MaxCalls)
    /\ rObj = None \/ rObj \in Clients \X (1..MaxCalls)
    /\ epAlive \in BOOLEAN
    /\ slots \in [1..NSlots -> {"E", "R", "G", "empty"}]
    /\ phase \in {"alive", "threads", "sweep", "done"}
    /\ sweep \in 1..(NSlots + 1)

Init ==
    /\ st = [c \in Clients |-> "run"]
    /\ calls = [c \in Clients |-> 0]
    /\ q = <<>>
    /\ srv = "run"
    /\ sReply = None
    /\ rObj = None
    /\ epAlive = TRUE
    /\ slots = InitSlots
    /\ phase = "alive"
    /\ sweep = 1

\* A reply capability <<c, n>> is live while caller c waits for call n.
Waiting(cap) == cap # None /\ st[cap[1]] = "reply" /\ calls[cap[1]] = cap[2]

\* Remove client c from the endpoint's queue.
Dequeue(c) == SelectSeq(q, LAMBDA x : x # c)

(*************************** client actions ******************************)

\* Call: rendezvous with a waiting server thread, or queue. A dead endpoint
\* fails at once (E_GONE), with no state change.
Call(c) ==
    /\ st[c] = "run" /\ calls[c] < MaxCalls /\ epAlive
    /\ calls' = [calls EXCEPT ![c] = @ + 1]
    /\ IF srv = "recvq"
         THEN /\ srv' = "run"
              /\ sReply' = <<c, calls[c] + 1>>
              /\ st' = [st EXCEPT ![c] = "reply"]
              /\ q' = q
         ELSE /\ st' = [st EXCEPT ![c] = "sendq"]
              /\ q' = Append(q, c)
              /\ UNCHANGED <<srv, sReply>>
    /\ UNCHANGED <<rObj, epAlive, slots, phase, sweep>>

\* A Call's deadline passes, queued or waiting for the reply (the timeout
\* covers the whole call). A reply capability naming it becomes stale.
Timeout(c) ==
    /\ st[c] \in {"sendq", "reply"}
    /\ st' = [st EXCEPT ![c] = "run"]
    /\ q' = Dequeue(c)
    /\ UNCHANGED <<calls, srv, sReply, rObj, epAlive, slots, phase, sweep>>

\* Pass a capability into the server's space (the stand-in for transfer in
\* messages). A destroyed space refuses; a dying one refuses only if
\* RefuseIntoDying.
Grant(c) ==
    /\ st[c] = "run"
    /\ phase # "done"
    /\ (RefuseIntoDying => phase = "alive")
    /\ \E i \in 1..NSlots :
         /\ slots[i] = "empty"
         /\ slots' = [slots EXCEPT ![i] = "G"]
    /\ UNCHANGED <<st, calls, q, srv, sReply, rObj, epAlive, phase, sweep>>

(*************************** server actions ******************************)

\* The server thread runs until step 2 destroys it, even after step 1 marked
\* its task dead: the model destroys threads, it doesn't freeze them.
Recv ==
    /\ srv = "run" /\ sReply = None /\ epAlive
    /\ IF q # <<>>
         THEN LET c == Head(q) IN
              /\ sReply' = <<c, calls[c]>>
              /\ st' = [st EXCEPT ![c] = "reply"]
              /\ q' = Tail(q)
              /\ srv' = srv
         ELSE /\ srv' = "recvq"
              /\ UNCHANGED <<sReply, st, q>>
    /\ UNCHANGED <<calls, rObj, epAlive, slots, phase, sweep>>

Reply ==
    /\ srv = "run" /\ sReply # None
    /\ st' = IF Waiting(sReply) THEN [st EXCEPT ![sReply[1]] = "run"] ELSE st
    /\ sReply' = None
    /\ UNCHANGED <<calls, q, srv, rObj, epAlive, slots, phase, sweep>>

\* reply_save into the reply object, if it still exists and is unbound.
Save ==
    /\ srv = "run" /\ sReply # None /\ rObj = None
    /\ \E i \in 1..NSlots : slots[i] = "R"
    /\ rObj' = sReply
    /\ sReply' = None
    /\ UNCHANGED <<st, calls, q, srv, epAlive, slots, phase, sweep>>

ReplySaved ==
    /\ srv = "run" /\ rObj # None
    /\ \E i \in 1..NSlots : slots[i] = "R"
    /\ st' = IF Waiting(rObj) THEN [st EXCEPT ![rObj[1]] = "run"] ELSE st
    /\ rObj' = None
    /\ UNCHANGED <<calls, q, srv, sReply, epAlive, slots, phase, sweep>>

(*********************** the preemptible destruction **********************)

\* Step 1: the task object is marked dead.
StartDestroy ==
    /\ phase = "alive"
    /\ phase' = "threads"
    /\ UNCHANGED <<st, calls, q, srv, sReply, rObj, epAlive, slots, sweep>>

\* Step 2: the server thread is destroyed: dequeued, and a caller it owes a
\* reply wakes with E_PEER (I10).
DestroyThread ==
    /\ phase = "threads"
    /\ srv' = "dead"
    /\ st' = IF Waiting(sReply) THEN [st EXCEPT ![sReply[1]] = "run"] ELSE st
    /\ sReply' = None
    /\ phase' = "sweep"
    /\ UNCHANGED <<calls, q, rObj, epAlive, slots, sweep>>

\* Step 3: delete one slot. Deleting the RECV capability kills the endpoint
\* and wakes its queue (E_GONE); deleting the reply object wakes its caller
\* (E_PEER). One slot per step: a preemption point between any two.
SweepStep ==
    /\ phase = "sweep"
    /\ IF sweep > NSlots
         THEN /\ phase' = "done"
              /\ UNCHANGED <<st, q, rObj, epAlive, slots, sweep>>
         ELSE LET s == slots[sweep] IN
              /\ slots' = [slots EXCEPT ![sweep] = "empty"]
              /\ sweep' = sweep + 1
              /\ phase' = phase
              /\ IF s = "E"
                   THEN /\ epAlive' = FALSE
                        /\ st' = [c \in Clients |-> IF st[c] = "sendq" THEN "run" ELSE st[c]]
                        /\ q' = <<>>
                        /\ rObj' = rObj
                   ELSE IF s = "R"
                     THEN /\ st' = IF Waiting(rObj) THEN [st EXCEPT ![rObj[1]] = "run"] ELSE st
                          /\ rObj' = None
                          /\ UNCHANGED <<epAlive, q>>
                     ELSE UNCHANGED <<st, q, rObj, epAlive>>
    /\ UNCHANGED <<calls, srv, sReply>>

Destroy == StartDestroy \/ DestroyThread \/ SweepStep

\* After destruction there may be nothing left to do. Saying so explicitly
\* keeps TLC's deadlock check on for every other state.
Terminated == phase = "done" /\ UNCHANGED vars

Next ==
    \/ \E c \in Clients : Call(c) \/ Timeout(c) \/ Grant(c)
    \/ Recv \/ Reply \/ Save \/ ReplySaved
    \/ Destroy
    \/ Terminated

\* Fairness: once started, destruction runs to the end. Nothing else is
\* assumed: clients and the server may stop at any time.
Spec == Init /\ [][Next]_vars /\ WF_vars(DestroyThread) /\ WF_vars(SweepStep)

(******************************* properties ******************************)

\* I6: a waiting caller is named by exactly one live reply capability.
I6 ==
    \A c \in Clients :
        st[c] = "reply" =>
            Cardinality({h \in {"thread", "object"} :
                           (h = "thread" /\ sReply = <<c, calls[c]>>)
                        \/ (h = "object" /\ rObj = <<c, calls[c]>>)}) = 1

\* I10, safety: a queued caller is on a live endpoint's queue, exactly once.
QueueSound ==
    /\ \A c \in Clients : (st[c] = "sendq") <=> (\E i \in 1..Len(q) : q[i] = c)
    /\ \A i, j \in 1..Len(q) : q[i] = q[j] => i = j
    /\ q # <<>> => epAlive

\* I1: when destruction ends, the space holds nothing.
SpaceEmptied == phase = "done" => \A i \in 1..NSlots : slots[i] = "empty"

\* I10, liveness: once destruction starts, it finishes and every client ends
\* up running, even those that called during it: nothing waits forever on a
\* dying server. (Afterwards the endpoint is dead, so nobody can block again.)
NoStrandedClient ==
    (phase = "threads") ~> (phase = "done" /\ \A c \in Clients : st[c] = "run")

=============================================================================
