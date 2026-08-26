# Concurrency

Design spec for yas's concurrency model: OS-thread parallelism with
channels as the message-passing primitive (CSP-style, in the Go/Erlang
tradition). This file is the agreed design; the `yas/concurrency` module
and its primitives land across the implementation milestones tracked in
the issue tracker, and the integration sections of the other docs
(ARCHITECTURE, LANGUAGE_REFERENCE, EMBEDDING, GC) are updated as each
lands.

## The model

yas runs one interpreter context per OS thread (see
[EMBEDDING.md](EMBEDDING.md) "Threads"): each context owns its heap,
intern tables, root stacks and registries, and atoms never cross
contexts - interning and `eq?` identity are context-local facts.
Concurrency builds directly on that wall:

- **Message passing only.** There is no shared mutable memory exposed to
  yas code: no locks, no atomics, no shared structures. A thread's only
  window onto another thread's data is a value that was explicitly sent
  through a channel, and what arrives is a copy.
- **Channels are the meeting point.** A channel is owned by neither
  context. Its buffer holds neutral (non-GC) copies of sent values; each
  context holds a handle, which is an ordinary local GC object.
- **Threads are spawned by name.** Because closures cannot cross
  contexts, a new thread receives code by re-resolving a name inside a
  fresh context, not by transferring a procedure value.

## Value transfer: the neutral marshal tree

Sending a value deep-copies it into a malloc'd tree of plain C structs
owned by the channel ("marshaled form"); receiving rebuilds a fresh value
in the receiving context's heap. Erlang-style copy semantics: the sender
keeps its original untouched, and no GC object is ever shared between
heaps.

Properties:

- **Symbols and keywords are re-interned** in the receiving context.
  Content survives; identity does not (`eq?` on a received symbol is true
  against the receiver's own interned instance, by construction).
- **Data is bit-exact**: integers, characters and string bytes copy
  verbatim; floats copy as raw bits (no text round-trip).
- **Structure sharing inside one message is preserved** (a DAG stays a
  DAG), and cyclic structure is supported: the marshaler keeps a
  visited map keyed on source-object identity and replays it on rebuild.
- **Channel handles may be sent.** Marshaling a handle increments the
  channel's reference count; the receiver gets a handle onto the same
  channel. This is how workers get handed their mailboxes.

What crosses, first cut:

| Crosses | Raises on send |
|---|---|
| nil, integers, floats, chars, strings, symbols, keywords, pairs, vectors, hashes, bytevectors, vec2/vec3/vec4, mat4, channel handles | procedures (closures, builtins, macros), continuations, ports, environments, frames, FFI values (libraries, pointers, functions, structs) |

Rejected types raise a catchable type error naming the offending type -
the failure is loud at the send site, never a silent truncation.

The existing `serialize`/`deserialize` pair in `yas/hash` is unrelated to
this mechanism (see [LANGUAGE_REFERENCE.md](LANGUAGE_REFERENCE.md),
"Serialization"): it is a textual tool with conversion quirks, and the
marshaler deliberately shares no code with it.

## Channels

```scheme
(define ch (make-channel 16))     ; bounded ring buffer, capacity 16

(channel-send ch v)               ; block while full; raise if closed
(channel-recv ch)                 ; block while empty-and-open
(channel-close ch)                ; idempotent; wakes all waiters
(channel-select 100 ch1 ch2)      ; first ready receive, or nil on timeout
```

Semantics:

- **Bounded only.** `make-channel` requires a positive integer capacity.
  Unbounded channels are deliberately not offered: backpressure is part
  of the model, and an unbounded buffer silently converts a producer that
  outruns its consumer into unbounded memory growth.
- **Send** blocks while the buffer is full; raises `channel-closed` if
  the channel is (or becomes) closed.
- **Recv** blocks while the buffer is empty and the channel is open;
  returns the oldest buffered value; raises `channel-closed` once the
  channel is closed *and* drained. Raising (rather than returning a
  sentinel) keeps `nil` a legal payload; callers use `try`/`catch` where
  close is expected.
- **Close** is idempotent and asynchronous-safe: blocked receivers wake
  to drain remaining buffered values then raise; blocked senders wake to
  raise immediately.
- **Select** waits for receive-readiness on any of its channels or for
  the timeout (milliseconds; `0` polls, blocking forever is not offered -
  use a plain recv). Returns `(value chosen-channel)` for the ready
  channel, or `nil` on timeout. Waiters on many channels lock them in
  memory-address order, so circular selects cannot deadlock. Send-side
  choice is deferred; add it only if a real workload demands it.
- Each channel is one pthread mutex plus two condition variables
  (not-full / not-empty) around its ring buffer - no lock ordering beyond
  select's address rule exists, because no operation ever holds two
  channel locks except select.

## Threads

```scheme
(define th (thread-start 'my/game 'worker (list ch 42)))
(define result (thread-join th))
```

- **`(thread-start module function args)`** spawns an OS thread that:
  1. creates a fresh interpreter context (as embedders do);
  2. auto-imports `yas/base` and `yas/concurrency`;
  3. imports `module`, which transitively loads its own dependencies
     through the normal module system;
  4. looks up `function` in the resulting environment and applies it to
     `args` (marshaled).
  `module` and `function` are symbols (or strings); `args` is one value.
  Spawn cost is dominated by loading the module graph - threads should be
  long-lived relative to their spawn, which suits the CSP style.
- **`(thread-join th)`** blocks until the thread finishes and returns its
  result, marshaled. If the thread died on an uncaught error, join
  re-raises it in the joining context (type and message preserved), so
  `try`/`catch` around `thread-join` is the error boundary for workers.
- **`(thread-yield)`** surrenders the CPU (`sched_yield`);
  **`(thread-sleep ms)`** sleeps.
- Unjoined threads simply run to completion; there is no detach API.
- Known hazards, documented rather than solved: calling `exit` (or
  returning from the main script) terminates the process regardless of
  live threads; concurrent `print` from several contexts writes through
  to the shared stdout, so individual calls cannot corrupt the stream but
  lines from different threads may interleave.

## GC interaction

The isolation wall makes the collector story small:

- **Marshaled payloads are invisible to both collectors.** They are pure
  malloc'd trees with no GC header, freed when their channel dies - never
  marked, never swept, never rooted.
- **Handles are ordinary per-context objects** (a dedicated atom type
  wrapping the shared channel pointer). They root normally, collect
  normally; a handle's release path decrements the channel refcount, and
  the last reference (handles across all contexts plus waiters' own
  references) frees the channel and its buffered payloads.
- **No cross-heap rooting exists anywhere.** Contexts destroy
  independently; destroying one releases its handles' references and
  leaves other contexts' handles valid.

## Layering

- `src/builtin_concurrency.c` - channel/thread builtins and the shared
  channel object; registered into a native `yas/concurrency` module like
  the other native modules.
- `src/marshal.c` - the neutral-tree codec (deep-copy in, rebuild out),
  independent of both the channel machinery and the evaluator so it can
  be reused later (e.g. for an embedding-level transfer API).
- `std/yas/concurrency.ys` - thin Lisp layer over the primitives; first
  cut adds little beyond documentation-friendly wrappers. Predicates
  (`channel?`, `thread?`) are C builtins.

## Testing plan

- **suite.ys**: deterministic single-context coverage - buffered
  send/recv round-trips, full-buffer and close semantics via
  `channel-select` polling, marshal round-trip equality (including
  cycles, shared substructure, bit-exact floats, re-interned symbols).
- **aux/channel_test.c**: C-level multi-threaded gate (producers and
  consumers across contexts asserting exact checksums, in the spirit of
  `aux/thread_test.c`), wired into `make test` and `make test-gc-stress`.
- **aux/concurrency_stress.ys**: N-producer/M-consumer and ping-pong
  workloads run through the interpreter, following the `*_stress.ys`
  convention (picked up automatically by `make stress-test` and the
  `test-gc-stress` loop).

## Documentation integration at landing

ARCHITECTURE.md gains a Concurrency section (channel object, marshaler,
spawn pipeline); LANGUAGE_REFERENCE.md lists the `yas/concurrency`
exports; EMBEDDING.md's "Threads" section drops its "no cross-context
transfer yet" caveat and describes the embedder-visible pieces; GC.md
notes payload neutrality and handle refcounting.
