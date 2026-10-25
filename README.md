# FDVSS and FDPSS

C++20 code for *Fully Dynamic Verifiable Secret Sharing and its Application to DPSS*.
Requires Clang or GCC, GNU Make, Bash, and Python 3.9+.

```sh
make native
make test
./scripts/reproduce.sh smoke
./scripts/reproduce.sh paper
```

Time is the sum of separately measured local stage computations on the all-honest
path. Communication counts aggregate payloads with one copy per broadcast.
New measurements are written to `results/` when the scripts run.

Error-path tests are included. See `fdvss/build/fdvss_reference --help`.
