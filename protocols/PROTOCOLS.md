# Protocols and implementation coverage

This document describes FDVSS-1, FDVSS-2, randomness generation (RG), and
redistribution (RD) from *Fully Dynamic Verifiable Secret Sharing and its
Application to DPSS*. The references are Figure 2, Figure 3, Algorithm 1, and
Algorithm 2, respectively. The final section identifies the paths exercised by
the experiment code; the abstract protocol descriptions are not a claim that
the artifact implements a complete distributed Byzantine protocol.

## Common conventions

- Each committee `Com_k` contains `n` parties `P_i^k`, for `i` in `[n]`.
  Assume `n >= 3t + 1`. Communication is synchronous, with authenticated private
  channels and a broadcast primitive between consecutive committees.
- The field is `F`, with `|F| > n`. Evaluation points are distinct and nonzero.
  The notation `i` in a polynomial evaluation denotes the public point
  `alpha_i`, not an integer coefficient. The implementation uses
  `alpha_i = i` for `i = 1, ..., n`.
- Every polynomial has degree at most `t` in each displayed variable unless
  specified otherwise. Polynomial records use fixed coefficient order and the
  prescribed degree bounds. Missing or malformed records are `BOTTOM`.
- `BW_V` returns a polynomial of degree at most `t` agreeing with at least
  `n-t` nonmissing positions, or `BOTTOM`. With vector- or polynomial-valued
  inputs, all components must agree at the same `n-t` positions. Separate
  componentwise support sets are insufficient.
- `Interp_V` interpolates a polynomial of degree at most `t` that agrees with
  every available position; it fails if fewer than `t+1` distinct positions
  are available or if the values are inconsistent.
- `Shamir.Share(s)` samples a random degree-at-most-`t` polynomial with constant
  term `s` and evaluates it at all `n` points. `Shamir.Rec` robustly decodes a
  polynomial with at least `n-t` supporters and returns its value at zero.
  Polynomial-valued sharing and reconstruction operate coefficientwise, with
  complete-record support for decoding.
- `MajorityRecover` requires at least `n-t` identical, well-formed broadcasts.
  It fails otherwise. This threshold is part of the protocol, even though the
  operation is named majority recovery.
- A failed inner reconstruction becomes an erasure for the next decoder.
  Empty vectors are valid objects, distinct from `BOTTOM`. A failed required
  public check disqualifies the dealer and selects the common zero sharing.
  Once disqualified, parties forward the status rather than executing further
  qualified-dealer steps.
- The paper's corruption bound applies to the union of positions corrupted
  during each committee's tenure, across all parallel instances. Honest roles
  erase obsolete private state after their outgoing work. These execution and
  erasure assumptions are not simulated by the local benchmark.

## FDVSS-1: five committee transitions

### Dealer in Com_0

1. Sample `F(W,X,Y,Z)` uniformly subject to `F(0,0,0,0) = m`.
2. Privately send `F(i,X,Y,Z)` to `P_i^1` for every `i`.

### Com_1: row distribution

Party `P_i^1` interprets its received row as `F^i(X,Y,Z)`, replacing a missing
or invalid row with the zero polynomial. It sends `F^i(j,Y,Z)` to `P_j^2`
for every `j`.

### Com_2: column decoding and raw forwarding

Each `P_j^2` performs the following steps.

1. Let `a_ij(Y,Z)` be the record from `P_i^1`. Compute
   `Q_j(W,Y,Z) = BW_W((a_ij)_i)`.
2. On success, set `I_j = {i: a_ij = BOTTOM or a_ij != Q_j(i,Y,Z)}`.
   On failure, set `I_j = [n]`. Broadcast `I_j` to `Com_3`.
3. For every `k`, send all raw values `(a_ij(k,Z))_i` to `P_k^3`, preserving
   erasures and including positions in `I_j`.

### Com_3: two-axis decoding and independent resharings

Each `P_k^3` performs the following steps.

1. Let `T_ijk(Z)` be the record received from `P_j^2` for row `i`. Compute

   ```text
   Qhat_j,k(W,Z) = BW_W((T_ijk)_i)
   U_i,k(X,Z)    = BW_X((T_ijk)_j)
   R_k(W,X,Z)   = BW_W((U_i,k)_i).
   ```

2. Replace a malformed complaint set by `[n]` and let
   `E = {(i,j): i in I_j}`. If `R_k` succeeded, set
   `v_k = (R_k(i,j,Z))_(i,j in E)`; otherwise set `v_k = BOTTOM`.
   For each `j`, set `d_j,k = (Qhat_j,k(i,Z))_(i in I_j)` if that decoder
   succeeded, and `d_j,k = BOTTOM` otherwise. Broadcast `(I_j)_j`, `v_k`, and
   `(d_j,k)_j` to `Com_4`.
3. For every `i`, independently Shamir-share `R_k(i,0,Z)` to `Com_4`, obtaining
   `h^0_ik,l(Z)` for recipient `P_l^4`. If `R_k` failed, send `BOTTOM` for
   these shares.
4. Independently Shamir-share each `U_i,k(0,Z)`, obtaining `h^1_ik,l(Z)` for
   `P_l^4`. If `U_i,k` failed, send `BOTTOM`. The two families of resharings
   use independent randomness, including when their secrets happen to agree.

### Com_4: public selection and forwarding

Each `P_l^4` performs the following steps from the common public transcript.

1. Majority-recover `(I_j)_j`, derive `E`, and compute
   `P = BW_Y((v_k)_k)`. Treat `BOTTOM` vectors as erasures. On failure,
   disqualify the dealer. Otherwise retain every complete supporter:
   `S = {k: v_k != BOTTOM and v_k = P(k,Z)}`. Require `|S| >= n-t`.
2. For each `j`, compute `C_j = BW_Y((d_j,k)_k)` and define

   ```text
   Good = {j: |I_j| <= t, C_j != BOTTOM,
              and C_j equals P restricted to I_j x {j}}.
   ```

   If `|Good| < 2t+1`, disqualify the dealer. Otherwise set
   `Happy = {i: |{j in Good: i not in I_j}| >= 2t+1}` and
   `Unhappy = [n] \ Happy`.
3. Broadcast the dealer's status. If qualified, also broadcast `S`, `Unhappy`,
   and `(h^0_ik,l(Z))_(i in Unhappy, k in [n])`. Privately send
   `z_ikl^(v) = h^1_ik,l(v)` for every `i,k` to `P_v^5`.

### Com_5: public repair and final shares

Each `P_v^5` performs the following steps.

1. Majority-recover the status. If disqualified, output zero. Otherwise
   majority-recover `S` and `Unhappy`, and set `Happy = [n] \ Unhappy`.
   Recover `a_ik(Z) = Shamir.Rec((h^0_ik,l(Z))_l)` for all `i in Unhappy`
   and `k in [n]`.
2. If `Unhappy` is empty, skip this step. Otherwise form the complete vector
   `a_k = (a_ik(Z))_(i in Unhappy)`, erasing the entire vector if `k not in S`
   or any component failed. Compute
   `(q_i(Y,Z))_(i in Unhappy) = BW_Y((a_k)_k)`, requiring at least `n-t`
   complete supporters. On failure, select the common zero sharing.
3. For each `i in Happy`, compute
   `r_i^(v) = Shamir.Rec((Shamir.Rec((z_ikl^(v))_l))_k)`.
   For each `i in Unhappy`, use `r_i^(v) = q_i(0,v)`.
   Output `[m]_v = Shamir.Rec((r_i^(v))_i)`. Propagate failed inner
   reconstructions as erasures; use zero if final reconstruction fails.

## FDVSS-2: three committee transitions

For the abstract protocol, define
`Dig_ij(p,rho) = H(instance, purpose, i, j, rho, coeff(p))`, where `H` is a
classical random oracle. The instance identifies the dealer's sharing
invocation. Use canonical encodings and independent salts. The paper uses
`kappa`-bit salts and oracle outputs; the concrete hash and encoding choices
in this artifact are stated below.

### Dealer in Com_0

1. Sample `F(W,X,Z)` uniformly subject to `F(0,0,0) = m`, and independent
   salts `rho_ij` for all `i,j`.
2. Compute `T_ij = Dig_ij(F(i,j,Z),rho_ij)` and broadcast the complete
   `n`-by-`n` digest table `T` to `Com_1`.
3. Privately send `(F(i,X,Z),(rho_ij)_j)` to each `P_i^1`.

### Com_1: authenticate the full row

Party `P_i^1` checks that its row, salts, and digest table are well formed and
that `Dig_ij(F^i(j,Z),rho_ij) = T_ij` for every `j`. If all checks pass, it
broadcasts `T` to `Com_2` and sends `(F^i(j,Z),rho_ij)` to each `P_j^2`.
Otherwise it broadcasts `BOTTOM` and sends `BOTTOM` to every recipient.

### Com_2: interpolate and check every authenticated slice

Each `P_j^2` performs the following steps.

1. Initialize `Q_j = BOTTOM` and `C_j = empty`. Recover a well-formed table
   `T` supported by at least `n-t` broadcasts, and let `A` contain all its
   senders. If no table qualifies, set `(T,A) = (BOTTOM,empty)` and skip to
   step 5.
2. For each received `(a_ij(Z),rho_ij)`, retain the record only if `i in A`,
   its encoding and degree are valid, and `Dig_ij(a_ij,rho_ij) = T_ij`.
   Treat every other record as `BOTTOM`. If fewer than `t+1` records remain,
   skip to step 5.
3. Let `B_j` contain the first `t+1` retained indices in public index order.
   Interpolate `Q_j(W,Z)` with `Q_j(i,Z) = a_ij(Z)` for every `i in B_j`,
   coefficientwise in `Z`.
4. Check `Q_j(i,Z) = a_ij(Z)` as a full polynomial identity for every
   retained index. If any check fails, choose the first failing index `i*`,
   set `C_j` to the `t+2` original authenticated records at
   `B_j union {i*}`, and reset `Q_j = BOTTOM`. Otherwise retain `Q_j` and
   the empty certificate. The failing index is outside `B_j` because the
   interpolant agrees with all interpolation records.
5. Broadcast `(T,A,C_j)` to `Com_3`. Privately send `z_j^(v) = Q_j(0,v)` to
   `P_v^3` if `Q_j != BOTTOM`, and send zero otherwise.

### Com_3: public certificate verification and final shares

Each `P_v^3` performs the following steps.

1. Majority-recover the complete pair `(T,A)`. If recovery fails, `T` is
   invalid, or `|A| < n-t`, disqualify the dealer and output zero.
2. Check every broadcast certificate `C_j`. A nonempty certificate is valid
   only if it contains exactly `t+2` distinct indices in `A`, every polynomial
   and salt is well formed and authenticates to `T_ij`, and no degree-at-most-`t`
   polynomial in `W` fits all its polynomial-valued records. This final test
   interpolates any `t+1` certificate records and checks the remaining record
   coefficientwise. If any certificate is valid, disqualify the dealer and
   select the common zero sharing. Ignore empty or invalid certificates.
3. If qualified, output `[m]_v = Shamir.Rec((z_j^(v))_j)`, using zero if
   reconstruction fails.

## RG: shared randomness generation

The default backend is FDVSS-2. Let the public Vandermonde matrix have
`M_i,k = alpha_i^(k-1)` for `i in [n]` and `k in [n-t]`.

1. Each `P_i^0` samples an independent uniform field element `m_i` and invokes
   FDVSS-2 as dealer. All `n` invocations run in parallel, use independent
   randomness and distinct instance identifiers, and deliver shares to `Com_3`.
2. Each `P_j^3` obtains `[m_1]_j, ..., [m_n]_j` and locally computes
   `[r_k]_j = sum_i M_i,k [m_i]_j` for `k = 1, ..., n-t`.
   A disqualified dealer contributes the default zero sharing.

The output is `n-t` shared random field elements. RD uses `t` of these elements
as fresh coefficients for one redistribution. These coefficients must not be
reused for another redistribution. Replacing FDVSS-2 by FDVSS-1 moves the RG
output to `Com_5`; the public linear transformation is unchanged.

## RD: one-transition redistribution

Suppose each `P_i^k` holds `[s]_i = f(i)` and independent random-coefficient
shares `[r_l]_i = R_l(i)` for `l = 1, ..., t`, where `f(0)=s` and
`R_l(0)=r_l`. All input sharings must be valid degree-at-most-`t` sharings.

1. For every target `j`, source party `P_i^k` privately sends
   `m_j,i = [s]_i + sum_(l=1)^t [r_l]_i alpha_j^l` to `P_j^(k+1)`.
2. Each target party robustly reconstructs
   `[s]'_j = Shamir.Rec((m_j,i)_i)`.

For fixed `j`, the received values are evaluations in `X` of
`H(X,alpha_j)`, where `H(X,Y) = f(X) + sum_l R_l(X)Y^l`.
The output is therefore an evaluation of
`g(Y) = H(0,Y) = s + sum_l r_l Y^l`.
With valid input sharings and at most `t` faulty senders, reconstruction
succeeds. The algorithm does not define a new valid sharing for arbitrary
invalid inputs; a reconstruction failure is an error outside that contract.

In the default FDPSS pipeline, initialization shares the secret with `Com_3`
using FDVSS-2. RG begins in each committee three transitions before its output
is needed. Once initialized, `Com_k` uses randomness from the RG invocation
started in `Com_(k-3)` to redistribute to `Com_(k+1)`, while starting a new RG
invocation for `Com_(k+3)`. The FDVSS-1 variant uses a five-transition horizon.

## Implementation coverage and measurement boundary

The source contains two execution paths: optimized local kernels for the
paper's all-honest measurements, and connected local reference executions
with explicit committee messages and failure branches. Both use deterministic
test inputs, not production protocol randomness. Neither is a network
deployment or a simulation of mobile corruption and secure erasure.

| Component | Exercised by the artifact | Boundary |
| --- | --- | --- |
| FDVSS-1 | Timed role kernels and a connected reference with complaint sets, complete-vector public decoding, all-support selection, Happy/Unhappy classification, independent resharings, public repair, and nested robust recovery | Most timed roles use separately prepared inputs. The full reference path is tested separately and is not the source of paper timing samples. |
| FDVSS-2 | Timed honest kernels and a connected reference with table/pair quorum, parsed row and slice messages, authenticated inconsistency witnesses, invalid-certificate rejection, and robust final recovery | Reference transcript encoding and counters are separate from the kernel ledger; transport is local ideal broadcast/private delivery. |
| RG | Timed FDVSS batch kernels and a reference that feeds actual outputs of n FDVSS-2 instances into the public Vandermonde transform, including rejected dealers' zero sharings | The timed implementation retains its separate algebraic output-contract harness. The connected reference is a distinct correctness execution. |
| RD | Timed local kernels and connected reference message construction with up to t wrong or missing source messages, robust decoding, and explicit failure propagation | There is no network scheduler or long-running mobile-corruption execution. |

Honest-path kernels can use precomputed interpolation weights and omit error
location searches because their inputs are generated consistently. This is
not a replacement for robust decoding on arbitrary received values. The
representative-node timing sums local stage work; it is neither the maximum
completion time of a committee nor distributed end-to-end latency. The total
time reported by the summarizer is the sum of these measured local kernels for
one run. Global `all_parties` validation stages are separate from that timing
measurement. Communication accounting reports aggregate modeled logical
payloads: point-to-point payloads and one native-broadcast copy for every
logical sender. Recipient-materialized broadcasts are reported separately.
The benchmark computes byte counts from canonical payload encodings; it does
not serialize messages onto sockets or measure transport time.

For the aggregate tables, the sender factors are one for the dealer stage and
`n` for each communicating committee stage. In the steady-state FDPSS table,
the representative transition ledger is multiplied by `n` because every
committee position executes the corresponding transition role. The raw
representative payload and the factor are retained in `stage-summary.csv`.

The concrete field is the Goldilocks prime `p = 2^64 - 2^32 + 1`. Field elements
occupy eight bytes; field operations reduce modulo `p`, with wider intermediate
arithmetic where needed. The FDVSS-2 code uses full 256-bit SHA-256 digests and
128-bit salts. Thus the code has distinct digest and salt lengths, rather than
128-bit digest outputs. Digest inputs include a fixed purpose string, a
benchmark instance identifier, zero-based row and column indices, the salt,
the coefficient count, and the coefficient vector. Integer fields are encoded
as fixed-width little-endian words; coefficients are canonical field residues.

The FDVSS-1 checks exercised by the artifact are deterministic: coefficient
vectors are compared coefficientwise when available, and otherwise all
prescribed evaluation points are checked. No randomized evaluation point or
randomized polynomial-identity test is used. The sampled output checksum is a
diagnostic and does not affect acceptance.

The benchmark uses SHA-256 as the concrete hash function, whereas the paper's
FDVSS-2 analysis assumes a classical random oracle and computationally bounded
adversaries. FDVSS-1's abstract information-theoretic claim does not extend to
deterministic benchmark randomness. Tests check the exercised arithmetic and
algebraic paths; they do not establish the security theorem or provide a
production deployment with reliable broadcast, private transport, secure
erasure, or adaptive-corruption handling.

### Reference failure-path execution

`fdvss_reference` runs the connected local protocol separately from
`fdvss_bench` and `fdpss_bench`. Reference decoding uses deterministic
Berlekamp-Welch linear systems per coordinate, then requires one complete
record supporter set of size at least `n-t`. It propagates failed inner
reconstructions as erasures. It performs no subset or clique search.

Fault scenarios alter outgoing messages before the next committee processes
them. Valid-budget scenarios corrupt at most `t` positions in each affected
committee. A malicious dealer may send inconsistent rows or invalid input
records to any recipients. Scenarios named `diagnostic` deliberately break a
committee's fault budget to exercise a failure branch; those cases are not
evidence of stronger resilience. The result's `verified` flag is a separate
test oracle over the final output and never affects protocol decisions.

Reference communication is aggregated over the actual local transcript.
Optional polynomial records carry a presence tag and coefficient counts;
sets and nested collections carry explicit counts. FDVSS-2 encodes and parses
framed little-endian byte messages, rejects noncanonical field elements,
invalid dimensions, and trailing data, and preserves the original authenticated
openings in certificates. FDVSS-1 uses typed polynomial records with modeled
counts for the corresponding reference format. These reference counters include
failure-path messages and are distinct from the paper's optimized all-honest
payload counters. Broadcast counters include one payload per sender and,
separately, recipient-materialized copies. A local transcript is not a
deployment of authenticated private transport or reliable broadcast.
