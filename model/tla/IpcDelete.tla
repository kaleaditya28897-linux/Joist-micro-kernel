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
(*   3. delete its capability slots one at a time;                         *)
(*   4. unmap its pages, a last-level table at a time (Phase 4, 0020:      *)
(*      here each page stands for one table).                              *)
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
(*   I12 when destruction ends, nothing is mapped in the space (Phase 4).  *)
(*                                                                         *)
(* RefuseIntoDying selects whether passing a capability into a space that  *)
(* is being destroyed is refused: one at a time, a batch at once           *)
(* (cap_copy_batch, 0022), or a mapping (0020). Without it, a capability   *)
(* can land in a slot the sweep has already passed and is never deleted    *)
(* (I1), and a page in a table it has passed is never unmapped (I12).      *)
(***************************************************************************)
EXTENDS Naturals, Sequences, FiniteSets

CONSTANTS Clients,          \* client threads, in one client task
          MaxCalls,         \* calls per client, to keep the state space finite
          NSlots,           \* slots in the server's space
          NPages,           \* pages (each a last-level table) of the server's space
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
    phase,    \* "alive", "threads" (task marked dead), "sweep", "tables", "done"
    sweep,    \* next slot the sweep deletes
    mapped,   \* the server's pages that are mapped (0020)
    tsweep    \* next page (table) the table sweep unmaps

pt == <<mapped, tsweep>>
vars == <<st, calls, q, srv, sReply, rObj, epAlive, slots, phase, sweep, mapped, tsweep>>

TypeOK ==
    /\ st \in [Clients -> {"run", "sendq", "reply"}]
    /\ calls \in [Clients -> 0..MaxCalls]
    /\ srv \in {"run", "recvq", "dead"}
    /\ sReply = None \/ sReply \in Clients \X (1..MaxCalls)
    /\ rObj = None \/ rObj \in Clients \X (1..MaxCalls)
    /\ epAlive \in BOOLEAN
    /\ slots \in [1..NSlots -> {"E", "R", "G", "empty"}]
    /\ phase \in {"alive", "threads", "sweep", "tables", "done"}
    /\ sweep \in 1..(NSlots + 1)
    /\ mapped \subseteq 1..NPages
    /\ tsweep \in 1..(NPages + 1)

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
    /\ mapped = {}
    /\ tsweep = 1

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
              /\ UNCHANGED <<srv, sReply, pt>>
    /\ UNCHANGED <<rObj, epAlive, slots, phase, sweep, pt>>

\* A Call's deadline passes, queued or waiting for the reply (the timeout
\* covers the whole call). A reply capability naming it becomes stale.
Timeout(c) ==
    /\ st[c] \in {"sendq", "reply"}
    /\ st' = [st EXCEPT ![c] = "run"]
    /\ q' = Dequeue(c)
    /\ UNCHANGED <<calls, srv, sReply, rObj, epAlive, slots, phase, sweep, pt>>

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
    /\ UNCHANGED <<st, calls, q, srv, sReply, rObj, epAlive, phase, sweep, pt>>

\* cap_copy_batch (0022): several capabilities at once, into empty slots,
\* in one step. Refused as Grant is.
CopyBatch(c) ==
    /\ st[c] = "run"
    /\ phase # "done"
    /\ (RefuseIntoDying => phase = "alive")
    /\ \E S \in SUBSET {i \in 1..NSlots : slots[i] = "empty"} :
         /\ S # {}
         /\ slots' = [i \in 1..NSlots |-> IF i \in S THEN "G" ELSE slots[i]]
    /\ UNCHANGED <<st, calls, q, srv, sReply, rObj, epAlive, phase, sweep, pt>>

\* Map a page into the server's space (the stand-in for mm, which controls
\* the task, 0020). Refused as Grant is: the kernel finds the task through
\* its task capability, which is dead from step 1 on.
Map(c) ==
    /\ st[c] = "run"
    /\ phase # "done"
    /\ (RefuseIntoDying => phase = "alive")
    /\ \E p \in (1..NPages) \ mapped : mapped' = mapped \cup {p}
    /\ UNCHANGED <<st, calls, q, srv, sReply, rObj, epAlive, slots, phase, sweep, tsweep>>

\* Unmap a page: allowed at any time (unmap_range takes holes).
Unmap(c) ==
    /\ st[c] = "run"
    /\ \E p \in mapped : mapped' = mapped \ {p}
    /\ UNCHANGED <<st, calls, q, srv, sReply, rObj, epAlive, slots, phase, sweep, tsweep>>

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
              /\ UNCHANGED <<sReply, st, q, pt>>
    /\ UNCHANGED <<calls, rObj, epAlive, slots, phase, sweep, pt>>

Reply ==
    /\ srv = "run" /\ sReply # None
    /\ st' = IF Waiting(sReply) THEN [st EXCEPT ![sReply[1]] = "run"] ELSE st
    /\ sReply' = None
    /\ UNCHANGED <<calls, q, srv, rObj, epAlive, slots, phase, sweep, pt>>

\* reply_save into the reply object, if it still exists and is unbound.
Save ==
    /\ srv = "run" /\ sReply # None /\ rObj = None
    /\ \E i \in 1..NSlots : slots[i] = "R"
    /\ rObj' = sReply
    /\ sReply' = None
    /\ UNCHANGED <<st, calls, q, srv, epAlive, slots, phase, sweep, pt>>

ReplySaved ==
    /\ srv = "run" /\ rObj # None
    /\ \E i \in 1..NSlots : slots[i] = "R"
    /\ st' = IF Waiting(rObj) THEN [st EXCEPT ![rObj[1]] = "run"] ELSE st
    /\ rObj' = None
    /\ UNCHANGED <<calls, q, srv, sReply, epAlive, slots, phase, sweep, pt>>

(*********************** the preemptible destruction **********************)

\* Step 1: the task object is marked dead.
StartDestroy ==
    /\ phase = "alive"
    /\ phase' = "threads"
    /\ UNCHANGED <<st, calls, q, srv, sReply, rObj, epAlive, slots, sweep, pt>>

\* Step 2: the server thread is destroyed: dequeued, and a caller it owes a
\* reply wakes with E_PEER (I10).
DestroyThread ==
    /\ phase = "threads"
    /\ srv' = "dead"
    /\ st' = IF Waiting(sReply) THEN [st EXCEPT ![sReply[1]] = "run"] ELSE st
    /\ sReply' = None
    /\ phase' = "sweep"
    /\ UNCHANGED <<calls, q, rObj, epAlive, slots, sweep, pt>>

\* Step 3: delete one slot. Deleting the RECV capability kills the endpoint
\* and wakes its queue (E_GONE); deleting the reply object wakes its caller
\* (E_PEER). One slot per step: a preemption point between any two.
SweepStep ==
    /\ phase = "sweep"
    /\ IF sweep > NSlots
         THEN /\ phase' = "tables"
              /\ UNCHANGED <<st, q, rObj, epAlive, slots, sweep, pt>>
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
                          /\ UNCHANGED <<epAlive, q, pt>>
                     ELSE UNCHANGED <<st, q, rObj, epAlive, pt>>
    /\ UNCHANGED <<calls, srv, sReply, pt>>

\* Step 4 (0020): unmap one page's table; then destruction is done. A
\* preemption point between any two, as for the slots.
TableStep ==
    /\ phase = "tables"
    /\ IF tsweep > NPages
         THEN /\ phase' = "done"
              /\ UNCHANGED <<mapped, tsweep>>
         ELSE /\ mapped' = mapped \ {tsweep}
              /\ tsweep' = tsweep + 1
              /\ phase' = phase
    /\ UNCHANGED <<st, calls, q, srv, sReply, rObj, epAlive, slots, sweep>>

Destroy == StartDestroy \/ DestroyThread \/ SweepStep \/ TableStep

\* After destruction there may be nothing left to do. Saying so explicitly
\* keeps TLC's deadlock check on for every other state.
Terminated == phase = "done" /\ UNCHANGED vars

Next ==
    \/ \E c \in Clients : Call(c) \/ Timeout(c) \/ Grant(c) \/ CopyBatch(c) \/ Map(c) \/ Unmap(c)
    \/ Recv \/ Reply \/ Save \/ ReplySaved
    \/ Destroy
    \/ Terminated

\* Fairness: once started, destruction runs to the end. Nothing else is
\* assumed: clients and the server may stop at any time.
Spec == Init /\ [][Next]_vars /\ WF_vars(DestroyThread) /\ WF_vars(SweepStep) /\ WF_vars(TableStep)

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

\* I12 (0020): when destruction ends, nothing is mapped in the space.
TablesEmptied == phase = "done" => mapped = {}

\* I10, liveness: once destruction starts, it finishes and every client ends
\* up running, even those that called during it: nothing waits forever on a
\* dying server. (Afterwards the endpoint is dead, so nobody can block again.)
NoStrandedClient ==
    (phase = "threads") ~> (phase = "done" /\ \A c \in Clients : st[c] = "run")

=============================================================================
