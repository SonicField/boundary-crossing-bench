use pyo3::prelude::*;

/// Rust linked list node exposed to Python via PyO3.
///
/// `frozen` eliminates the borrow tracking entirely — no AtomicUsize CAS
/// per access. `get()` returns `&T` via direct pointer dereference.
///
/// `Sync` is required by `get()`. `RustNode` is trivially Sync because
/// it contains no interior mutability (frozen guarantees this).
#[pyclass(frozen)]
struct RustNode {
    #[pyo3(get)]
    value: i64,
    #[pyo3(get)]
    next: Option<Py<RustNode>>,
}

#[pymethods]
impl RustNode {
    #[new]
    #[pyo3(signature = (value, next=None))]
    fn new(value: i64, next: Option<Py<RustNode>>) -> Self {
        RustNode { value, next }
    }
}

/// Sum all values in a RustNode linked list.
///
/// Optimised PyO3 pattern for frozen pyclass:
/// - `bind(py).get()` returns `&RustNode` via pointer dereference (no borrow tracking)
/// - No isinstance check per node (type is `Py<RustNode>`, known statically)
/// - `clone_ref` (Py_INCREF) per node is unavoidable — we need an owned handle to advance
///
/// Remaining per-node overhead vs C: clone_ref (INCREF) + drop old current (DECREF).
#[pyfunction]
fn rust_sum_list(head: &Bound<'_, PyAny>) -> PyResult<i64> {
    let py = head.py();
    let mut total: i64 = 0;

    if head.is_none() {
        return Ok(0);
    }

    // Type check once at entry — not per node
    let first: &Bound<'_, RustNode> = head.cast()?;
    let mut current: Py<RustNode> = first.clone().unbind();

    loop {
        // get(): direct pointer dereference, no borrow tracking, no PyRef guard
        let node: &RustNode = current.bind(py).get();
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

#[pymodule]
fn rust_node(m: &Bound<'_, PyModule>) -> PyResult<()> {
    m.add_class::<RustNode>()?;
    m.add_function(wrap_pyfunction!(rust_sum_list, m)?)?;
    Ok(())
}
