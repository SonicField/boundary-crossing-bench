# Contributing

Thank you for your interest in this project.

## Project Status

This repository is a snapshot of a specific experiment measuring
boundary-crossing overhead in CPython extensions. It is not under active
development. If further research is conducted, the repository may be
updated.

## How to Contribute

### Reporting Issues

If you find an error in the methodology, results, or code, please open an
issue with:

- A clear, descriptive title
- What you believe is incorrect and why
- Evidence or reasoning supporting your claim
- Your environment details if reproducing a benchmark discrepancy

### Reproducing Results

If you reproduce the benchmarks on different hardware and get meaningfully
different results, we would be interested to hear about it. Please include:

- Full hardware specification (CPU model, L1 cache size)
- Python, Rust, and GCC/Clang versions
- Raw benchmark output
- Any modifications you made to the code

### Pull Requests

Pull requests are welcome for:

- Bug fixes in the benchmark code
- Documentation corrections
- Additional controlled experiments that extend the analysis

Before submitting a large PR, consider opening an issue first to discuss the
approach.

## Building

### C extension

```bash
cd c_node
cc -shared -fPIC -O3 -DNDEBUG -I$(python3 -c "import sysconfig; print(sysconfig.get_path('include'))") \
    c_node.c -o ../c_node.cpython-$(python3 -c "import sys; print(f'{sys.version_info.major}{sys.version_info.minor}')")-$(python3 -c "import sysconfig; print(sysconfig.get_config_var('MULTIARCH'))").so
```

Or using setuptools:

```bash
cd c_node && pip install -e .
```

### Rust extension

```bash
cd rust_node && maturin develop --release
```

Or manually:

```bash
cd rust_node && cargo build --release
cp target/release/librust_node.so ../rust_node.cpython-<version>-<arch>.so
```

### Running

```bash
python3 bench.py
```

## License

By contributing, you agree that your contributions will be licensed under
the MIT License.
