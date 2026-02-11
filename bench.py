"""Boundary-crossing benchmark: linked list traversal.

Measures the cost of field access across three implementations:
  - Pure Python (dataclass)
  - C extension (direct struct access)
  - Rust/PyO3 (extract + borrow protocol)

Each implementation provides both a Node type and a native sum_list function.
A cross-language test uses a single Python sum_list on all node types.
"""

import platform
import subprocess
import sys
import time

from python_node import PyNode, py_sum_list

# Import C and Rust extensions
from c_node import CNode, c_sum_list
from rust_node import RustNode, rust_sum_list

N = 1000       # list length
M = 100_000    # iterations


def build_list(NodeClass, n):
    """Build a linked list of n nodes with values 0..n-1."""
    assert n > 0, f"List length must be positive, got {n}"
    head = NodeClass(value=n - 1, next=None)
    for i in range(n - 2, -1, -1):
        head = NodeClass(value=i, next=head)
    return head


def python_sum_list(head):
    """Python traversal — same code, any node type.

    Uses Python's attribute protocol (LOAD_ATTR) to access .value and .next.
    This goes through different paths depending on the underlying type:
      - Python dataclass: LOAD_ATTR specialised by CPython
      - C extension: PyMemberDef descriptor
      - Rust/PyO3: #[pyo3(get)] descriptor (__get__ method)
    """
    total = 0
    current = head
    while current is not None:
        total += current.value
        current = current.next
    return total


def bench(label, fn, head, iterations):
    """Run a benchmark with warmup and timing."""
    assert iterations > 0, f"Iterations must be positive, got {iterations}"
    assert callable(fn), f"fn must be callable, got {type(fn)}"

    # Warmup — let CPython specialise LOAD_ATTR etc.
    for _ in range(1000):
        fn(head)

    # Timed run
    t0 = time.perf_counter_ns()
    for _ in range(iterations):
        fn(head)
    elapsed_ns = time.perf_counter_ns() - t0

    ns_per = elapsed_ns / iterations
    print(f"{label:40s}  {ns_per:8.0f} ns/traversal")
    return ns_per


def get_compiler_version():
    """Get the C compiler version used to build CPython."""
    try:
        result = subprocess.run(
            ["cc", "--version"],
            capture_output=True, text=True, timeout=5,
        )
        return result.stdout.split("\n")[0].strip()
    except Exception:
        return "unknown"


def get_rust_version():
    """Get the Rust compiler version."""
    try:
        result = subprocess.run(
            ["rustc", "--version"],
            capture_output=True, text=True, timeout=5,
        )
        return result.stdout.strip()
    except Exception:
        return "unknown"


def main():
    # --- Environment ---
    print("=" * 60)
    print("Boundary Crossing Benchmark")
    print("=" * 60)
    print(f"Python:   {sys.version}")
    print(f"Platform: {platform.platform()}")
    print(f"CC:       {get_compiler_version()}")
    print(f"Rust:     {get_rust_version()}")
    print()

    # --- Build lists ---
    py_list = build_list(PyNode, N)
    c_list = build_list(CNode, N)
    rust_list = build_list(RustNode, N)

    # --- Correctness verification ---
    expected = N * (N - 1) // 2
    assert py_sum_list(py_list) == expected, \
        f"py_sum_list wrong: {py_sum_list(py_list)} != {expected}"
    assert c_sum_list(c_list) == expected, \
        f"c_sum_list wrong: {c_sum_list(c_list)} != {expected}"
    assert rust_sum_list(rust_list) == expected, \
        f"rust_sum_list wrong: {rust_sum_list(rust_list)} != {expected}"
    assert python_sum_list(py_list) == expected, \
        f"python_sum_list(py) wrong: {python_sum_list(py_list)} != {expected}"
    assert python_sum_list(c_list) == expected, \
        f"python_sum_list(c) wrong: {python_sum_list(c_list)} != {expected}"
    assert python_sum_list(rust_list) == expected, \
        f"python_sum_list(rust) wrong: {python_sum_list(rust_list)} != {expected}"
    print(f"Correctness: all implementations produce {expected} "
          f"(sum 0..{N-1})")
    print()

    # --- Benchmark ---
    print(f"Linked list traversal: {N} nodes, {M:,} iterations")
    print(f"{'Benchmark':40s}  {'ns/traversal':>14s}")
    print("-" * 56)

    # Native implementations (loop + data in same language)
    print("\n--- Native (loop + data in same language) ---")
    py_native = bench("Python loop, Python nodes", py_sum_list, py_list, M)
    c_native = bench("C loop, C nodes", c_sum_list, c_list, M)
    rust_native = bench("Rust loop, Rust nodes", rust_sum_list, rust_list, M)

    # Cross-language (Python loop, different node types)
    print("\n--- Python loop, different node types ---")
    py_cross = bench("Python loop, Python nodes", python_sum_list, py_list, M)
    c_cross = bench("Python loop, C nodes", python_sum_list, c_list, M)
    rust_cross = bench("Python loop, Rust nodes", python_sum_list, rust_list, M)

    # --- Summary ratios ---
    print("\n--- Ratios (relative to C native) ---")
    print(f"  Python native / C native:  {py_native / c_native:6.2f}x")
    print(f"  Rust native / C native:    {rust_native / c_native:6.2f}x")
    print(f"  Python cross / C native:   {py_cross / c_native:6.2f}x")
    print(f"  C cross / C native:        {c_cross / c_native:6.2f}x")
    print(f"  Rust cross / C native:     {rust_cross / c_native:6.2f}x")

    # --- Falsification check ---
    print("\n--- Falsification ---")
    if rust_native < c_native:
        print("UNEXPECTED: Rust native FASTER than C native.")
        print("The PyO3 boundary-crossing hypothesis is WRONG for this workload.")
    else:
        overhead_ns = (rust_native - c_native) / N
        print(f"Rust native slower than C native by "
              f"{rust_native - c_native:.0f} ns/traversal "
              f"({overhead_ns:.1f} ns/node).")
        print("Consistent with PyO3 extract/borrow overhead per node.")


if __name__ == "__main__":
    main()
