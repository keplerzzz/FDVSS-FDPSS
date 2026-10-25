# Protocol implementation

FDVSS-1, FDVSS-2, RG, and RD follow the paper's protocol figures and algorithms.
Evaluation points are 1 through n, with n > 3t.

- Arithmetic uses the Goldilocks prime 2^64 - 2^32 + 1.
- FDVSS-2 uses SHA-256 truncated to 128 bits and 128-bit salts.
- Polynomial checks are deterministic, with common complete-record support.
- Failed public checks select the zero sharing; failed inner recovery is an erasure.
- RG supplies the first t masks to one RD transition without reuse.

The benchmark measures local stages separately and sums their times per run.
Connected executions are retained for correctness and error-path tests.
Payload counters use encoded messages; each broadcast is counted once per sender.
Test inputs use reproducible seeds. No recorded measurements are included.
