# FDVSS and FDPSS

Experiment code for *Fully Dynamic Verifiable Secret Sharing and its
Application to DPSS*.

## Requirements

C++20 compiler (Clang or GCC) with `unsigned __int128` support, GNU Make,
Bash, and Python 3.9 or later.

## Build and test

```sh
make native
make test
```

Tests include small-scale error-path cases. Optional sanitizer checks:
`make sanitize`.

## Run experiments

```sh
./scripts/reproduce.sh smoke
./scripts/reproduce.sh paper
```

The paper configuration uses `n = 8,16,32,64,128` and
`t = floor((n-1)/3)`. Generated samples and summaries are saved under `results/`.
Settings can be overridden, for example:

```sh
SIZES=8,16,32 THREADS=4 REPEATS=10 ./scripts/reproduce.sh paper
```

Performance measurements use the all-honest path. Time measures local
computation on one machine. Summary communication sizes are aggregate modeled
payloads, with each broadcast counted once per sender.

## Protocol reference

The separate local reference implementation exercises normal and error paths:

```sh
./fdvss/build/fdvss_reference --protocol both --n 4 --scenario all
```

See [protocol descriptions](protocols/PROTOCOLS.md) for implementation details.
