# Joist native ABI

**Status: skeleton.** Phase 1's exit deliverable (plan §11). This file is
written *from the host model's API* (`model/`) as Phase 1 settles it, not
ahead of it. Until then each section only states what the accepted decisions
already fix, and leaves the parameters open.

Invariant numbers (I1–I10) refer to `docs/phase1-plan.md`.

## Objects

| Object | Fixed by | Open until Phase 1 settles it |
|---|---|---|
| Task | 0004, 0011, 0013 | creation interface, how memory is donated |
| Thread | 0004, 0009, 0010 | register-state record layout; TLS base mechanism (0010) |
| Capability | 0001 | rights encoding; **badge width and allocation** (0001 item 2); capability-space representation (item 4) |
| Endpoint | 0001 | — |
| Notification object | 0001, 0005, 0014 | **bit-set width** (0001 item 3) |
| Reply capability | 0001 | — |

## IPC operations

`Send`, `Recv`, `Call`, `Reply`, `ReplyRecv`, `Wait` (0001). Every blocking
form takes a timeout (§5.3). Open: the number and width of message
registers (0001 item 1), and whether `Send` survives (item 5).

*Per operation, to be filled from the model:* arguments, rights required,
result codes, blocking behaviour, the bound on its work (I9), and which
invariants it can affect.

## Capability operations

Derive (rights never grow, I8), mint badge (I7), delete, revoke (bounded,
may block the caller, never allocates: 0003, I9), and the batched
capability-space copy `fork` needs (0011).

## Lifetime notifications

No-senders per badge, preallocated accounting (I4); dead-name on endpoint
destruction (I5) (§5.4, 0001).

## Error codes

*To be filled from the model.*
