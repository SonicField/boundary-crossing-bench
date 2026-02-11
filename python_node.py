"""Pure Python linked list node and traversal."""

from dataclasses import dataclass
from typing import Optional


@dataclass
class PyNode:
    value: int
    next: Optional["PyNode"]


def py_sum_list(head: Optional[PyNode]) -> int:
    """Sum all values in a linked list. Pure Python implementation."""
    total = 0
    current = head
    while current is not None:
        total += current.value
        current = current.next
    return total
