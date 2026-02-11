"""Build configuration for c_node C extension."""

from setuptools import setup, Extension

setup(
    name="c_node",
    version="0.1.0",
    ext_modules=[
        Extension(
            "c_node",
            sources=["c_node.c"],
        ),
    ],
)
