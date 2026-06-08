# Design: rcuref-inspired Scalable pthread_rwlock rdunlock

## 1. Problem Statement

glibc's `__pthread_rwlock_rdunlock` uses a CAS (compare-and-swap) retry loop
on the `__readers` word to atomically decrement the reader count AND
manipulate flag bits when the count reaches zero.  Under N concurrent reader
unlocks, all N threads target the same cache line with CAS:

```
Thread A: CAS(__readers, 0x18, 0x10) → succeeds
Thread B: CAS(__readers, 0x18, 0x10) → FAILS (A changed it), reload, retry
Thread C: CAS(__readers, 0x18, 0x10) → FAILS, reload, retry
...
```

Failed CAS attempts must reload and retry.  In the worst case, N-1 threads
fail per round, giving O(N) rounds × O(N) threads = **O(N²) aggregate atomic
operations**.  On a 160-core machine this causes measurable throughput
degradation at high core counts.

## 2. `__readers` Bitmap Layout

```
 Bit 31                              Bit 3  Bit 2  Bit 1  Bit 0
┌─────────────────────────────────────┬──────┬──────┬──────┬──────┐
│       Reader Count (29 bits)        │  RW  │  WL  │  WP  │      │
│                                     │      │      │      │      │
└─────────────────────────────────────┴──────┴──────┴──────┴──────┘
```

| Symbol | Bit(s) | Name | Meaning |
|--------|--------|------|---------|
| WP | 0 | `PTHREAD_RWLOCK_WRPHASE` | 1 = write phase; 0 = read phase |
| WL | 1 | `PTHREAD_RWLOCK_WRLOCKED` | 1 = primary writer exists |
| RW | 2 | `PTHREAD_RWLOCK_RWAITING` | 1 = readers blocked (writer-preferred) |
| R | 3..31 | Reader count | Number of registered readers |

Constants:
```c
#define PTHREAD_RWLOCK_WRPHASE          1       // 0x01
#define PTHREAD_RWLOCK_WRLOCKED         2       // 0x02
#define PTHREAD_RWLOCK_RWAITING         4       // 0x04
#define PTHREAD_RWLOCK_READER_SHIFT     3
#define PTHREAD_RWLOCK_READER_OVERFLOW  (1U << 31)
#define PTHREAD_RWLOCK_FUTEX_USED       (1U << 30)  // in __wrphase_futex
```

Arithmetic on the reader count uses `(1 << PTHREAD_RWLOCK_READER_SHIFT) = 0x8`
as the unit.  Adding/subtracting 0x8 modifies only bits 3..31, **never
touching the flag bits 0..2**.  This is the fundamental property that enables
our optimization.

## 3. Valid States

The rwlock has 8 meaningful states (from glibc's documentation):

| State | WP | WL | R   | RW | Description |
|-------|----|----|-----|----|-------------|
| #1    | 0  | 0  | 0   | 0  | Idle, read phase |
| #2    | 0  | 0  | >0  | 0  | Readers hold the lock |
| #3    | 0  | 1  | 0   | 0  | Writer acquired WRLOCKED, no readers, attempting write phase |
| #4    | 0  | 1  | >0  | 0  | Readers hold; writer waiting for handover |
| #4a   | 0  | 1  | >0  | 1  | Same + new readers blocked (writer-preferred) |
| #5    | 1  | 0  | 0   | 0  | Idle, write phase |
| #6    | 1  | 0  | >0  | 0  | Write phase; readers attempting read phase |
| #7    | 1  | 1  | 0   | 0  | Writer holds the lock |
| #8    | 1  | 1  | >0  | 0  | Writer holds; readers waiting for handover |

## 4. State Transitions Relevant to rdunlock

Reader unlock decrements R.  When R reaches 0, flag transitions may be needed:

### 4.1 Normal read unlock (R > 1 → R - 1 > 0)

```
State #2 (R=N, WP=0, WL=0, RW=0)  →  State #2 (R=N-1)
State #4 (R=N, WP=0, WL=1, RW=0)  →  State #4 (R=N-1)
State #4a(R=N, WP=0, WL=1, RW=1)  →  State #4a(R=N-1)
```

**No flag changes needed.**  This is the common case (fast path).

### 4.2 Last reader unlock, no writer (R=1 → R=0, WL=0)

```
State #2 (R=1, WP=0, WL=0, RW=0)  →  State #1 (R=0, WP=0, WL=0, RW=0)
```

**No flag changes needed.**  Lock becomes idle.

### 4.3 Last reader unlock, writer waiting (R=1 → R=0, WL=1)

```
State #4 (R=1, WP=0, WL=1, RW=0)  →  State #7 (R=0, WP=1, WL=1, RW=0)
  Actions: set WRPHASE=1, wake writer via __wrphase_futex

State #4a(R=1, WP=0, WL=1, RW=1)  →  State #7 (R=0, WP=1, WL=1, RW=0)
  Actions: set WRPHASE=1, clear RWAITING=0, wake writer, wake blocked readers
```

**Flag changes required.**  The last reader hands ownership to the writer.

### 4.4 Transition summary for rdunlock

| From | Condition | To | Flags changed | Who must do it |
|------|-----------|-----|---------------|----------------|
| #2 (R>1) | R-1 > 0 | #2 (R-1) | none | any reader |
| #2 (R=1) | R-1=0, WL=0 | #1 | none | last reader |
| #4 (R>1) | R-1 > 0 | #4 (R-1) | none | any reader |
| #4 (R=1) | R-1=0, WL=1 | #7 | WP: 0→1 | last reader |
| #4a(R>1) | R-1 > 0 | #4a(R-1) | none | any reader |
| #4a(R=1) | R-1=0, WL=1 | #7 | WP: 0→1, RW: 1→0 | last reader |

**Key observation:** Flag changes are ONLY needed when R transitions from 1 to 0
AND WL=1.  In all other cases, the count decrement alone is sufficient.

## 5. Original Algorithm (CAS Loop)

```c
unsigned int r = atomic_load_relaxed(&__readers);
unsigned int rnew;
for (;;) {
    rnew = r - (1 << READER_SHIFT);           // decrement count
    if ((rnew >> READER_SHIFT) == 0) {        // if we'd make count 0
        if ((rnew & WRLOCKED) != 0)
            rnew |= WRPHASE;                  // set write phase
        rnew &= ~RWAITING;                    // clear reader-waiting
    }
    if (CAS(&__readers, &r, rnew)) break;     // atomic update
    // On failure: r is reloaded, retry
}
// Post-CAS: wake writer if WRPHASE was set, wake readers if RWAITING cleared
```

**Cost analysis:**  Under N concurrent unlocks, each CAS can fail because
another thread modified `__readers` between the load and CAS.  With N threads
racing:
- Round 1: 1 succeeds, N-1 fail and retry
- Round 2: 1 succeeds, N-2 fail and retry
- ...
- Total attempts: N + (N-1) + (N-2) + ... + 1 = **N(N+1)/2 = O(N²)**

## 6. New Algorithm (Unconditional fetch_add)

```c
unsigned int old = atomic_fetch_add_release(&__readers,
    -(1 << READER_SHIFT));

if (old >> READER_SHIFT > 1)
    return;    // fast path: not last reader, done

// Slow path: we brought count from 1 to 0.  Handle flags.
unsigned int r = atomic_load_relaxed(&__readers);
unsigned int rnew;
for (;;) {
    if ((r >> READER_SHIFT) != 0)
        return;                               // new reader arrived, it takes over
    rnew = r;
    if ((rnew & WRLOCKED) != 0)
        rnew |= WRPHASE;
    rnew &= ~RWAITING;
    if (rnew == r)
        return;                               // no flag changes needed
    if (CAS(&__readers, &r, rnew)) break;
}
// Post-CAS: wake writer/readers as needed
```

**Cost analysis:**
- The `fetch_add` always succeeds in one shot — no retry.  N threads do N ops = **O(N)**.
- Only ONE thread sees `old >> SHIFT == 1` and enters the slow path.
- The slow-path CAS is uncontended (only one thread is here).
- Total: N unconditional decrements + 1 CAS = **O(N)**.

## 7. Correctness Proof

### 7.1 Property: fetch_add never corrupts flag bits

`fetch_add(&__readers, -(1 << 3))` subtracts 8 from the word.  Since the
reader count occupies bits 3..31 and starts at a value ≥ 1 (the thread must
hold a read lock), the subtraction borrows from bit 3 upward.  It can never
underflow into bits 0..2 because:

- Minimum value of R before unlock = 1 (the calling thread itself)
- 1 << 3 = 8 in the count field
- Subtracting 8 from a word where bits 3..31 encode at least 1 (= 8 raw)
  yields 0 in bits 3..31.  Bits 0..2 are unaffected.

**Invariant:** For any thread calling rdunlock, `__readers >> SHIFT >= 1` at
the moment of fetch_add, because the thread incremented the count in rdlock
and has not yet decremented it.

### 7.2 Property: Exactly one thread observes old_count == 1

`atomic_fetch_add` returns the value BEFORE the operation.  If N readers are
simultaneously unlocking from count=N:

- Thread that executes first: sees old=N (encoded), `old >> SHIFT = N > 1` → fast path
- Thread that executes second: sees old=N-1, `old >> SHIFT = N-1 > 1` → fast path
- ...
- Thread that executes last: sees old=1, `old >> SHIFT = 1` → enters slow path

The atomicity of fetch_add guarantees no two threads see the same "old" value.
**Exactly one thread sees old_count == 1.**

### 7.3 Property: The slow-path thread correctly handles all flag transitions

When the slow-path thread enters, the count in `__readers` is already 0
(the fetch_add brought it there).  The thread loads `__readers` and checks:

1. **If count > 0:** A new reader arrived between our fetch_add and our load.
   That reader now holds the lock, and when it eventually unlocks, IT will be
   responsible for the 1→0 transition.  We return safely.

2. **If count == 0 and WL == 0:** No writer waiting.  State is #1 (idle).
   `rnew == r` → no CAS needed, return.

3. **If count == 0 and WL == 1:** Writer waiting.  We CAS to set WRPHASE
   (and clear RWAITING if set).  This transitions from state #4/#4a to #7.
   Then we wake the writer via `__wrphase_futex`.

### 7.4 Race Analysis: Concurrent new reader during flag fixup

```
Timeline:
  T1 (last reader):  fetch_add → old=1 (count becomes 0)
  T2 (new reader):   fetch_add → count becomes 1  [rdlock path]
  T1:                load __readers → sees count=1
  T1:                (r >> SHIFT) != 0 → return
```

**Safe.** T1 observes that T2 is now a reader and defers responsibility.
T2 will handle the transition when it eventually unlocks.

### 7.5 Race Analysis: Writer observes count=0 before WRPHASE is set

```
Timeline:
  T1 (last reader):  fetch_add → count becomes 0
  Writer:            already sleeping on __wrphase_futex (entered at wrlock time)
  T1:                CAS sets WRPHASE=1
  T1:                writes __wrphase_futex = 1
  T1:                calls futex_wake
  Writer:            wakes up, proceeds with write phase
```

**Safe.** The writer does NOT poll `__readers` while waiting.  It blocks on
`__wrphase_futex` in a futex_wait loop.  The transient state where count=0
but WRPHASE=0 is invisible to the writer because:

- The writer entered its wait loop BEFORE setting WRPHASE (it set WRLOCKED,
  then if readers were present, it blocks on `__wrphase_futex`).
- The writer's `futex_wait(&__wrphase_futex, FUTEX_USED)` will only return
  when someone writes a non-FUTEX_USED value to `__wrphase_futex`.
- T1 sets `__wrphase_futex = 1` after setting WRPHASE, then calls wake.

### 7.6 Race Analysis: Writer arrives AFTER count reaches 0 but BEFORE WRPHASE set

```
Timeline:
  T1 (last reader):  fetch_add → count becomes 0, flags = [WP=0, WL=0, RW=0]
  Writer:            fetch_or(WRLOCKED) → __readers = 0x02 [count=0, WL=1, WP=0]
  Writer:            sees count=0, enters while loop to CAS WRPHASE on
  Writer:            CAS(__readers, 0x02, 0x03) → succeeds, sets WRPHASE
  Writer:            stores __wrphase_futex = 1
  Writer:            goto done (acquired)
  T1 (slow path):   load __readers → 0x03 [count=0, WL=1, WP=1]
  T1:                count=0, computes rnew = 0x03 | WRPHASE = 0x03
  T1:                rnew == r → return (nothing to do)
```

**Safe.** The writer handled its own transition because it saw count=0 at
wrlock time (line 839-858 in glibc: writer CAS-sets WRPHASE if count==0).
T1's slow path sees WRPHASE already set and exits harmlessly.

### 7.7 Race Analysis: Multiple fast-path threads, count doesn't reach 0

```
Initial: count=4, WL=1 (state #4)
  A: fetch_add → old=4 (raw 0x22), count 4→3, old>>3=4 > 1 → return
  B: fetch_add → old=3 (raw 0x1A), count 3→2, old>>3=3 > 1 → return
  C: fetch_add → old=2 (raw 0x12), count 2→1, old>>3=2 > 1 → return
  D: fetch_add → old=1 (raw 0x0A), count 1→0, old>>3=1 → slow path

  D enters slow path, loads __readers = 0x02 [count=0, WL=1, WP=0]
  D: CAS(0x02, 0x03) → sets WRPHASE, wakes writer ✓
```

**Safe.** The ordering of A, B, C, D is serialized by the atomic fetch_add.
Regardless of which physical thread gets which position, exactly one sees
old_count=1.

### 7.8 Race Analysis: fetch_add interleaved with writer's fetch_or(WRLOCKED)

```
Initial: count=2, WL=0 (state #2), __readers = 0x10
  A: fetch_add(-8) → old=0x10, __readers = 0x08, old>>3=2 > 1 → return
  Writer: fetch_or(WRLOCKED) → __readers = 0x08|0x02 = 0x0A [count=1, WL=1]
  B: fetch_add(-8) → old=0x0A, __readers = 0x02, old>>3=1 → slow path

  B loads __readers = 0x02 [count=0, WL=1, WP=0]
  B: CAS(0x02, 0x03) → sets WRPHASE, wakes writer ✓
```

**Safe.** B correctly sees WL=1 (writer arrived between A's and B's unlock)
and hands over to the writer.

### 7.9 Race Analysis: Reader and writer arrive simultaneously during flag fixup

```
Initial: count=1, WL=1 (state #4), __readers = 0x0A
  T1 (reader): fetch_add(-8) → old=0x0A, __readers = 0x02, old>>3=1 → slow path
  T2 (new reader): fetch_add(+8) → __readers = 0x0A [count=1, WL=1]
  T1 slow path: load __readers → 0x0A, count=1
  T1: (r >> SHIFT) != 0 → return

  Later, T2 unlocks:
  T2: fetch_add(-8) → old=0x0A, __readers = 0x02, old>>3=1 → slow path
  T2: load → 0x02 [count=0, WL=1, WP=0]
  T2: CAS(0x02, 0x03) → sets WRPHASE, wakes writer ✓
```

**Safe.** T1 defers to T2; T2 eventually handles the transition.

### 7.10 Race Analysis: RWAITING flag handling

```
Initial: count=1, WL=1, RW=1 (state #4a), __readers = 0x0E
  T1: fetch_add(-8) → old=0x0E, __readers = 0x06, old>>3=1 → slow path
  T1: load → 0x06 [count=0, WL=1, WP=0, RW=1]
  T1: rnew = 0x06 | WRPHASE = 0x07; rnew &= ~RWAITING → 0x03
  T1: CAS(0x06, 0x03) → succeeds
  T1: WRPHASE set → wake writer via __wrphase_futex
  T1: r & RWAITING != rnew & RWAITING → wake readers on __readers futex ✓
```

**Safe.** Both WRPHASE transition and RWAITING clearing happen in the single
uncontended CAS.

## 8. Memory Ordering Argument

| Operation | MO | Justification |
|-----------|-----|---------------|
| fetch_add in rdunlock | release | Ensures all prior reads (critical section) are visible before we release the lock |
| load in slow path | relaxed | We only need to read flags; the release on fetch_add already published our CS |
| CAS in slow path | release | Writer synchronizes with this via acquire on __wrphase_futex |
| exchange on __wrphase_futex | relaxed | Used as a futex word; ordering provided by __readers operations |
| fence_acquire (before futex wake) | acquire | Ensures we see the writer's prior stores when transitioning to write phase |

The fetch_add with release MO is **identical** to the release MO on the
original CAS.  No ordering is weakened.

## 9. Liveness Argument

**Claim:** The new algorithm is wait-free on the fast path, and lock-free on
the slow path.

- **Fast path (old >> SHIFT > 1):** Single atomic, always succeeds.  No loops.
  Wait-free.

- **Slow path (old >> SHIFT == 1):** The CAS loop can retry only if:
  1. A new reader arrives (bumps count from 0 to >0) — we detect this and
     return (the new reader takes over responsibility).
  2. The writer CAS-sets WRPHASE before us — we detect `rnew == r` and return.

  In both cases, progress is made by SOME thread (either the new reader or
  the writer).  This gives **lock-freedom**: at least one thread makes progress
  per step.  In practice, since only one thread enters the slow path and
  interference comes from useful work (new reader or writer acquiring), the
  CAS typically succeeds on the first attempt.

**No deadlock:** The original deadlock concern (nobody sets WRPHASE when count
reaches 0) is eliminated because:
- Only one thread can bring count from 1→0 (atomic fetch_add serialization)
- That thread is unconditionally responsible for flag fixup
- If a new reader "steals" responsibility by incrementing count, the new
  reader will eventually unlock and handle it

## 10. Performance Model

| Metric | Original (CAS loop) | New (fetch_add) |
|--------|---------------------|-----------------|
| Atomic ops per unlock (no contention) | 1 CAS | 1 fetch_add |
| Atomic ops per unlock (N contending) | O(N) retries avg | 1 fetch_add |
| Aggregate ops for N unlocks | O(N²) | O(N) + 1 CAS |
| Cache line bounces | O(N²) (each retry bounces) | O(N) (each fetch_add bounces once) |
| Slow path frequency | Always (all threads use CAS) | 1/N (only last thread) |
| Slow path contention | High (all N threads) | Zero (one thread) |

## 11. Benchmark Validation

160-core Intel Xeon, pure-reader workload, 256-iteration critical section:

```
Threads   System glibc    Patched         Improvement
-------   ------------    -------         -----------
1         1.64 Mops/s     1.64 Mops/s     —
16        15.62           15.68           —
32        15.09           15.37           +2%
64        11.81           14.89           +26%
96        11.24           13.68           +22%
128       10.35           12.98           +25%
160       9.84            13.89           +41%
```

System glibc degrades from 15.6 to 9.8 Mops/s (37% throughput loss).
Patched glibc sustains 13-15 Mops/s across all thread counts.

Minimal critical section (rdunlock-stress, isolates unlock overhead):

```
Threads   System glibc    Patched         Improvement
-------   ------------    -------         -----------
32        10.25           12.69           +24%
64        10.73           13.38           +25%
96        9.50            10.92           +15%
128       9.81            10.91           +11%
160       8.24            10.82           +31%
```

## 12. Limitations and Non-goals

1. **Single-reader case (N=1):** No improvement.  Both algorithms do one
   atomic op.  Slight overhead from the conditional branch (predicted taken).

2. **Write-heavy workloads:** Not affected.  Writer unlock path is unchanged.
   The optimization targets reader unlock specifically.

3. **Two readers:** Marginal improvement.  The O(N²) cost is only significant
   at N >> 2.

4. **tryrdlock unchanged:** tryrdlock already uses a CAS loop for acquisition
   (to atomically set count and potentially flip WRPHASE).  Its unlock goes
   through the same rdunlock path and benefits from this optimization.

## 13. Failed Approaches

### 13.1 Two-variable hybrid (__reader_count + __readers)

Added a separate `__reader_count` field.  rdunlock decrements `__reader_count`
first; if prev > 1, unconditional fetch_sub on `__readers`.  Only prev==1
thread enters CAS loop.

**Failure mode:** Correctness race.  The slow-path CAS could execute before
fast-path fetch_subs completed, causing count to reach 0 without flag handling.
Even after fixing with a recovery mechanism, the **performance was worse** —
the extra atomic on `__reader_count` adds a second cache line bounce per
lock/unlock cycle, costing more than the CAS retries it eliminates.

### 13.2 Spin-wait for count==1 before CAS

The slow-path thread (prev==1 from `__reader_count`) would spin until
`__readers >> SHIFT == 1`, then do the final CAS 1→0 with flags.

**Failure mode:** Violates lock-freedom.  If a fast-path thread is preempted
between its `__reader_count` decrement and its `__readers` fetch_sub, the
slow-path thread spins indefinitely.

## 14. Conclusion

The unconditional `atomic_fetch_add` approach is correct, performant, and
minimal:

- **1 file changed**, net -2 lines vs original
- **No new struct fields**, no ABI changes
- **O(N) aggregate** cost vs O(N²) for original
- **Wait-free fast path**, lock-free slow path
- **All 416 nptl tests pass**
- **+41% throughput** at 160 cores

The design leverages the fundamental property that `__readers`'s count field
(bits 3..31) is arithmetically independent of the flag bits (0..2), allowing
unconditional addition without risk of flag corruption.  Flag manipulation is
deferred to a single uncontended CAS executed by exactly one thread — the one
whose atomic decrement observes old_count == 1.
