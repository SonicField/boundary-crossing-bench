# Boundary Crossing Benchmark: C vs Rust/PyO3 Extension Performance

Measuring the real cost of PyO3's safety abstractions for fine-grained
field access in CPython extensions.

## Status

This repository is a snapshot of a specific experiment, not a library or
framework. It is not intended to be built upon at this time. If further
research is conducted — different hardware, free-threaded Python,
newer PyO3 versions — the repository may be updated.

## Repository Structure

```
.
├── bench.py                  # Benchmark harness (main entry point)
├── python_node.py            # Pure Python baseline (dataclass)
├── c_node/
│   ├── c_node.c              # C extension with GC tracking (48 bytes/node)
│   ├── c_node_nogc.c         # C extension without GC tracking (32 bytes/node)
│   └── setup.py              # setuptools build config
└── rust_node/
    ├── src/lib.rs             # Rust/PyO3 extension (optimised: frozen + get())
    ├── Cargo.toml             # Rust dependencies (PyO3 0.27.2)
    └── pyproject.toml         # maturin build config
```

## Building and Running

See [CONTRIBUTING.md](CONTRIBUTING.md) for detailed build instructions.

Quick start (assuming CPython, Rust, and a C compiler are installed):

```bash
# Build C extension
cd c_node
cc -shared -fPIC -O3 -DNDEBUG \
    -I$(python3 -c "import sysconfig; print(sysconfig.get_path('include'))") \
    c_node.c -o ../c_node.$(python3 -c "import importlib.machinery; print(importlib.machinery.EXTENSION_SUFFIXES[0])")
cd ..

# Build Rust extension
cd rust_node && maturin develop --release && cd ..

# Run benchmark
python3 bench.py
```

## License

[MIT](LICENSE)

---

# The Three Lines Between Slow and Fast in PyO3

## The hypothesis

There is a reasonable hypothesis that Rust/PyO3 extensions are slower
than C extensions for fine-grained operations because PyO3's safety
abstractions add overhead on every field access. Naive implementations
appear to support this — and it is easy to stop there.

I set out to measure it carefully. The result was more interesting than
the hypothesis.

## The experiment

A linked list of 1,000 nodes. Each node holds an integer `value` and a
pointer `next`. Sum all values by traversing the list. Repeat 100,000
times. Report nanoseconds per traversal.

Three implementations of the same data structure and the same traversal:

- **Pure Python**: `@dataclass` nodes, Python `while` loop
- **C extension**: `PyObject_HEAD` struct, pointer dereference in C
- **Rust/PyO3**: `#[pyclass]` struct, `.extract()` in Rust

Same workload, same machine, same Python interpreter.

### Environment

All x86\_64 results were collected on:

| Component | Version |
|-----------|---------|
| CPU | Intel Xeon Platinum 8339HC @ 1.80 GHz (L1d: 32 KiB/core) |
| Python | 3.15.0a2+ (CPython) |
| GCC | 11.5.0 |
| Rust | 1.91.0 (LLVM) |
| Clang | 21.1.7 (LLVM) |
| Platform | Linux x86\_64 |

ARM results (see [Cross-architecture](#cross-architecture-arm-neoverse-v2))
were collected on:

| Component | Version |
|-----------|---------|
| CPU | NVIDIA Grace (Neoverse-V2) (L1d: 64 KiB/core) |
| Python | 3.12.12+meta (CPython) |
| GCC | 11.5.0 |
| Rust | 1.93.0 (LLVM) |
| Platform | Linux aarch64 |

## Round 1: the naive result

### C extension (the fast one, supposedly)

```c
typedef struct {
    PyObject_HEAD
    long value;
    PyObject *next;
} NodeObject;

static PyObject *
c_sum_list(PyObject *self, PyObject *head)
{
    long total = 0;
    PyObject *current = head;
    while (current != Py_None) {
        total += ((NodeObject *)current)->value;
        current = ((NodeObject *)current)->next;
    }
    return PyLong_FromLong(total);
}
```

Two pointer dereferences per node. Compiles to four x86 instructions.

### Rust extension (the slow one, supposedly)

```rust
#[pyclass]
struct RustNode {
    #[pyo3(get)]
    value: i64,
    #[pyo3(get)]
    next: Option<Py<PyAny>>,
}

#[pyfunction]
fn rust_sum_list(head: &Bound<'_, PyAny>) -> PyResult<i64> {
    let py = head.py();
    let mut total: i64 = 0;
    let mut current: Option<Py<PyAny>> = if head.is_none() {
        None
    } else {
        Some(head.clone().unbind())
    };
    while let Some(ref node_py) = current {
        let node: PyRef<'_, RustNode> = node_py.bind(py).extract()?;
        total += node.value;
        current = node.next.as_ref().map(|n| n.clone_ref(py));
    }
    Ok(total)
}
```

This is the idiomatic PyO3 pattern from tutorials and documentation.
Every node access goes through `.extract::<PyRef<RustNode>>()`.

### Round 1 results

```
Python loop, Python nodes        38,553 ns/traversal
C loop, C nodes                   3,202 ns/traversal
Rust loop, Rust nodes            22,823 ns/traversal
```

Rust is **7.1× slower than C**. Claim confirmed? Almost published
that. Then I looked at what `.extract()` actually does.

## What extract() costs per node

The PyO3 source (v0.27.2) reveals the per-node cost of
`bound.extract::<PyRef<T>>()`:

1. **`cast::<T>()`** — calls `isinstance()` (type pointer comparison +
   potential MRO walk)
2. **`try_borrow()`** — atomic compare-and-swap on a borrow counter
   (`lock cmpxchg` on x86)
3. **`PyRef` construction** — `Py_INCREF` on the underlying object
4. **`PyRef` drop** — atomic decrement on the borrow counter +
   `Py_DECREF` on the object

That is two atomic operations and two reference count changes per node,
plus a type check. For a loop body that is two pointer dereferences,
this overhead is the entire cost.

But none of it is necessary for this workload.

## The three changes

### 1. `#[pyclass(frozen)]`

`RustNode` is immutable after construction. Marking it `frozen`
eliminates borrow tracking entirely. PyO3 replaces the `BorrowChecker`
(an `AtomicUsize`) with `EmptySlot` — a zero-sized type whose
`try_borrow()` is an unconditional `Ok(())`. This is resolved at
compile time through monomorphisation. No runtime branch. No atomic
operation. No memory.

```rust
#[pyclass(frozen)]  // was: #[pyclass]
struct RustNode {
```

### 2. `Option<Py<RustNode>>` instead of `Option<Py<PyAny>>`

The `next` field was `Py<PyAny>` — a type-erased Python object handle.
Every access required `extract()` to cast it back to `RustNode`,
repeating the isinstance check on every node.

Changing the field type to `Py<RustNode>` makes the type statically
known. The isinstance check happens once at function entry, not per
node.

```rust
    #[pyo3(get)]
    next: Option<Py<RustNode>>,  // was: Option<Py<PyAny>>
```

### 3. `get()` instead of `extract()`

For frozen `Sync` classes, PyO3 provides `Bound::get()` which returns
`&T` via a direct pointer dereference. No `PyRef` guard, no
INCREF/DECREF for the wrapper, no atomic operations. The implementation
is:

```rust
// PyO3 source, instance.rs
pub fn get(&self) -> &T
where
    T: PyClass<Frozen = True> + Sync,
{
    unsafe { &*self.get_class_object().get_ptr() }
}
```

That compiles to the same pointer arithmetic as the C version.

### The optimised Rust code

```rust
#[pyclass(frozen)]
struct RustNode {
    #[pyo3(get)]
    value: i64,
    #[pyo3(get)]
    next: Option<Py<RustNode>>,
}

#[pyfunction]
fn rust_sum_list(head: &Bound<'_, PyAny>) -> PyResult<i64> {
    let py = head.py();
    let mut total: i64 = 0;
    if head.is_none() {
        return Ok(0);
    }
    let first: &Bound<'_, RustNode> = head.cast()?; // type check once
    let mut current: Py<RustNode> = first.clone().unbind();
    loop {
        let node: &RustNode = current.bind(py).get(); // pointer deref
        total += node.value;
        match node.next {
            Some(ref next) => {
                let next_owned = next.clone_ref(py);
                current = next_owned;
            }
            None => break,
        }
    }
    Ok(total)
}
```

No `unsafe`. No `#[allow(...)]`. No circumventing PyO3's safety model.
These are documented, stable PyO3 features used as intended.

## Round 2 results

```
Python loop, Python nodes        40,198 ns/traversal
C loop, C nodes                   3,146 ns/traversal
Rust loop, Rust nodes             2,550 ns/traversal
```

Rust is now **faster than C**. The 22,823 ns became 2,550 ns — an
**8.9× improvement** from three annotation changes.

But this result is misleading, and publishing it without investigation
would be dishonest.

## Why Rust appeared faster: the cache effect

The C extension uses `Py_TPFLAGS_HAVE_GC`, which prepends a 16-byte
`PyGC_Head` to each object for cyclic garbage collection tracking. The
Rust extension does not enable GC tracking by default (`#[pyclass]`
does not set the GC flag). This changes the object sizes:

| Type | Object size | 1,000 nodes |
|------|------------|-------------|
| CNode (GC) | 48 bytes | 47 KB |
| RustNode | 32 bytes | 31 KB |

L1 data cache on this machine: 32 KiB per core (Intel Xeon 8339HC).

The C list overflows L1. The Rust list fits. That is the difference.

### Controlling for object size

To verify, I built a second C extension identical to the first but
without GC tracking. Two lines change in the type definition:

```c
/* Original — GC-tracked, 48 bytes per object */
static PyTypeObject NodeType = {
    ...
    .tp_flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC,
    .tp_dealloc = (destructor)Node_dealloc,  /* calls PyObject_GC_UnTrack */
    .tp_traverse = (traverseproc)Node_traverse,
    .tp_clear = (inquiry)Node_clear,
    ...
};

/* Modified — no GC tracking, 32 bytes per object */
static PyTypeObject NodeNoGCType = {
    ...
    .tp_flags = Py_TPFLAGS_DEFAULT,          /* no Py_TPFLAGS_HAVE_GC */
    .tp_dealloc = (destructor)NodeNoGC_dealloc,  /* just Py_XDECREF + tp_free */
    /* no tp_traverse, no tp_clear */
    ...
};
```

The struct, the traversal function, and the compiled loop body are
identical. Only the allocation path changes: without `HAVE_GC`, CPython
does not prepend the 16-byte `PyGC_Head`, so each object is 32 bytes —
the same as `RustNode`.

This is a legitimate comparison: neither the C no-GC type nor
`RustNode` participates in cyclic GC. If the linked list forms a cycle,
both will leak. For a benchmark where the list is acyclic and the
structure is fully controlled, GC tracking is pure overhead.

### The result

```
C (with GC, 48 bytes/node)       3,065 ns  (3.1 ns/node)
C (no GC, 32 bytes/node)         2,030 ns  (2.0 ns/node)
Rust (no GC, 32 bytes/node)      2,560 ns  (2.6 ns/node)
```

With equal object sizes, **C is 1.27× faster than Rust**. The
remaining 0.6 ns/node gap is the `Py_INCREF` + `Py_DECREF` that Rust
performs on every node to maintain an owned handle while advancing the
loop. C follows raw pointers with no reference counting in the hot
path:

```c
/* C: no refcount changes in the loop */
while (current != Py_None) {
    total += ((NodeObject *)current)->value;
    current = ((NodeObject *)current)->next;   /* raw pointer copy */
}
```

```rust
// Rust: INCREF next, then DECREF old current, on every iteration
loop {
    let node: &RustNode = current.bind(py).get();
    total += node.value;
    match node.next {
        Some(ref next) => {
            let next_owned = next.clone_ref(py);  // Py_INCREF
            current = next_owned;                  // old current dropped → Py_DECREF
        }
        None => break,
    }
}
```

Rust needs `clone_ref` because `node.next` is borrowed from `current`
— you cannot advance to the next node while still borrowing the current
one. C has no such constraint: `current = ...->next` is a pointer copy
with no ownership implications. The GIL guarantees nothing is
deallocated during the traversal, so the raw pointer is safe in
practice even though it carries no proof of that safety in the type
system.

This 0.6 ns/node is the irreducible cost of PyO3's ownership model. It
cannot be removed without `unsafe` code.

### Compiler control

To rule out compiler quality as a variable, I built the C extension
with both GCC 11.5 and Clang 21.1 (the same LLVM backend Rust uses).
Both produced identical loop bodies and identical performance:

```
C (GCC, no GC)                   2,030 ns
C (Clang/LLVM, no GC)            2,030 ns
Rust (LLVM)                      2,560 ns
```

The difference is not the compiler. It is the INCREF/DECREF.

### Cross-architecture: ARM Neoverse-V2

To test whether these results are architecture-specific, I ran the same
benchmark on an NVIDIA Grace CPU (ARM Neoverse-V2, aarch64) with 64 KiB
L1d cache per core — double the Intel machine's 32 KiB.

```
--- Native (loop + data in same language) ---
Python loop, Python nodes        23,216 ns/traversal
C loop, C nodes                   1,301 ns/traversal
Rust loop, Rust nodes             1,803 ns/traversal
```

Controlled comparison (3 runs, stable to within 3 ns):

```
C (GC, 48 bytes/node)           1,312 ns  (1.3 ns/node)
C (no GC, 32 bytes/node)        1,303 ns  (1.3 ns/node)
Rust (no GC, 32 bytes/node)     1,812 ns  (1.8 ns/node)
```

Two predictions from the x86\_64 results are confirmed:

1. **The GC-size cache effect disappeared.** On Intel (32 KiB L1d),
   GC-tracked C was 55% slower than no-GC C (3.1 vs 2.0 ns/node).
   On ARM (64 KiB L1d), they are identical (1.3 vs 1.3 ns/node). Both
   the 47 KB and 31 KB lists fit in the larger L1. The paper's
   "What would invalidate these results" section predicted this.

2. **The INCREF/DECREF gap persists.** C is 1.39× faster than Rust
   on ARM (vs 1.27× on Intel). The absolute gap is 0.5 ns/node (vs
   0.6 ns/node on Intel). This cost is structural — it follows from
   PyO3's ownership model, not from any architecture-specific behaviour.

| | Intel x86\_64 | ARM aarch64 |
|---|---|---|
| C (no GC) | 2.0 ns/node | 1.3 ns/node |
| Rust (frozen) | 2.6 ns/node | 1.8 ns/node |
| C/Rust ratio | 1.27× | 1.39× |
| GC-size effect | 55% slowdown | none |

## The full picture

Stable results across 5 runs (x86\_64) and 3 runs (aarch64):

| Implementation | ns/node (x86\_64) | ns/node (aarch64) | What it does per node |
|----------------|-------------------|--------------------|-----------------------|
| C (no GC) | 2.0 | 1.3 | 2 loads, 1 add, 1 compare |
| Rust (frozen) | 2.6 | 1.8 | 2 loads, 1 add, 1 compare, INCREF, DECREF |
| C (with GC) | 3.1 | 1.3 | Same as C-no-GC but 48-byte objects (L1 miss on x86\_64) |
| Rust (naive) | 22.8 | — | 2 loads, 1 add, 1 compare, isinstance, atomic CAS ×2, INCREF ×2, DECREF ×2 |
| Python | 38.6 | 23.2 | LOAD\_ATTR specialisation, bytecode dispatch |

The naive Rust code was not 7× slower because of a fundamental
architectural problem. It was 7× slower because every node access paid
for safety checks that the data structure did not need.

## Cross-language: accessing Rust nodes from Python

There is a second benchmark: a Python `while` loop accessing `.value`
and `.next` on each node type through Python's attribute protocol:

```
Python loop, Python nodes        39,811 ns
Python loop, C nodes             59,874 ns
Python loop, Rust nodes          91,621 ns
```

The `frozen` optimisation helps here too (down from 120,149 ns), but
Rust nodes accessed from Python are still 2.3× slower than Python
dataclass nodes. This is because `#[pyo3(get)]` goes through a Python
descriptor, while CPython's `LOAD_ATTR` for dataclass fields is
specialised in bytecode. This cross-language cost is real and is not
affected by the three changes above.

## What this means

The performance story of PyO3 is not "Rust is slow" or "C is fast". It
is:

1. **Default patterns have hidden costs.** `#[pyclass]` defaults to
   mutable (borrow tracking on every access). `Py<PyAny>` erases type
   information (isinstance check on every access). `extract()` combines
   both costs. These defaults are correct for the general case — they
   prevent data races and type confusion. But they are not free.

2. **PyO3 provides the tools to eliminate those costs.** `frozen`,
   `get()`, and typed `Py<T>` fields are stable, documented, safe API.
   They exist precisely for the case where you know your data is
   immutable and your types are fixed.

3. **The irreducible floor is INCREF/DECREF.** With all safety overhead
   removed, the remaining cost is one reference count increment and one
   decrement per object traversed — 0.6 ns/node on x86\_64, 0.5 ns/node
   on aarch64. This is PyO3's ownership model: Rust does not have raw
   access to Python's reference graph. Whether this matters depends on
   the workload. For most applications, 0.5–0.6 ns/node is noise. For a
   tight interpreter loop touching millions of objects per second, it is
   the difference between C and Rust.

4. **Object size matters more than instruction count.** The GC-tracked
   C objects (48 bytes) were slower than Rust's non-GC objects (32 bytes)
   despite C executing fewer instructions per node. L1 cache is 32 KB.
   Fifty per cent more memory per object means fifty per cent more cache
   misses. Measure before you reason.

## Methodology

All source code is in this repository.

- **Warmup**: 1,000 iterations before timing
- **Iterations**: 100,000 timed iterations per benchmark
- **Timer**: `time.perf_counter_ns()` (monotonic, nanosecond resolution)
- **Verification**: All implementations produce the same sum (499,500)
  with assertion checks before benchmarking
- **Stability**: Each key result verified across 3–5 independent runs
- **Compiler parity**: C tested with both GCC and Clang to isolate
  compiler effects
- **Object size parity**: C tested with and without GC tracking to
  isolate cache effects
- **Rust build**: `--release` (optimised), PyO3 0.27.2
- **C build**: `-O3 -DNDEBUG`
- **Python build**: CPython 3.15.0a2+, standard (GIL-enabled) build

### Falsification criteria (stated before running)

"If Rust native traversal is faster than C native traversal, the PyO3
boundary-crossing hypothesis is wrong for this workload."

The naive Rust was 7.1× slower — hypothesis held. The optimised Rust
was faster, but only because of a confound (GC object size / L1 cache).
With the confound removed (equal object sizes, no GC), C is 1.27×
faster — consistent with the INCREF/DECREF overhead prediction.

### What would invalidate these results

- ~~Running on hardware with larger L1 cache (>48 KB) would eliminate the
  GC-size effect~~ — **confirmed**: on ARM Neoverse-V2 (64 KiB L1d),
  GC and no-GC C performance is identical (1.3 ns/node both)
- Free-threaded Python (`--disable-gil`) would make INCREF/DECREF atomic,
  increasing Rust's per-node cost
- A list longer than ~8,000 nodes would overflow L1 for both
  implementations, changing the relative costs
- PyO3 versions newer than 0.27.2 may change the `get()` or borrow
  tracking implementation
