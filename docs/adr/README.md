# Architecture decision records

These records describe implemented choices, checked against the repository on
2026-10-14. Accepted means the project uses the decision; it does not imply an
external architecture board approval or standards compliance.

| Record | Decision |
| --- | --- |
| [0001](0001-behavior-interfaces.md) | Callback ECU behavior and small injectable interfaces |
| [0002](0002-bounded-routing.md) | Synchronous ID-filtered fan-out with bounded per-receiver FIFO queues |
| [0003](0003-worker-ownership.md) | One owner per ECU; separate deterministic and concurrent execution |
| [0004](0004-time-and-diagnostics.md) | Injected monotonic time and separate diagnostic detection/storage |
| [0005](0005-local-http.md) | Optional HTTP dashboard adapter over a managed C++ JSON-lines process |
| [0006](0006-build-and-ci.md) | CMake targets, pinned test dependencies and an OS CI matrix |

Source relationships and execution order are shown in [the UML](../uml.md).
Execution evidence, skill assessment and remaining gaps are in
[the final review](../final-review.md).
