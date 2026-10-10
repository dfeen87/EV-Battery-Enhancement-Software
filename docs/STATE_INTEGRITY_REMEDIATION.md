# BEDROCK state integrity remediation: PB-02, PB-01, PB-03

Baseline: main `e4f0417d48d9ed217ed3d4b0cfe3363e85a0199c` (merged PR #38,
v8.0.1). This phase changes only the three state-integrity findings, in dependency
order PB-02, PB-01, PB-03. Historical release reports remain unchanged.

## Reproductions and corrections

| Finding | Reproduced on main | After correction |
| --- | --- | --- |
| PB-02 | `400 V`, `10 A`, SOC `0.8`: update for `1 s`, then `DBL_MAX s` throws, leaving time `1.79769e308`, throughput `inf`, and an invalid state. | The exception leaves time `1 s`, throughput `10/3600 Ah`, and every prior scalar, gradient and metric element unchanged. |
| PB-01 | Two NMC cells updated at `{3.7,3.7} V`, `{25,25} C`, `10 A`, `1 s`; `{3.8,NaN} V`, `{30,30} C`, `20 A`, `1 s` throws after times become `{2,1} s`, voltages `{3.8,NaN}`, while cached pack voltage is `7.4 V`. | Rejects the sensor set before cell computation; times remain `{1,1} s`, voltages `{3.7,3.7}`, current `10 A`, and all prior statistics remain intact. |
| PB-03 | `enhance(360,0,25,0.8,1)` followed by `enhance(400,0,25,0.5,0.001)` returns stability true, new `400 V` / SOC `0.5`, old time `1 s`, and old physical energy `77760000 J` rather than `54000000 J`. | The short cycle throws `std::invalid_argument`; committed `360 V`, SOC `0.8`, time `1 s`, energy `77760000 J` are preserved. A subsequent supported cycle with `400 V` / SOC `0.5` computes `54000000 J`. |

`DSCoupling::update` now computes time, informational history, gradients, metric,
and energies in a local state and assigns the caller's state only after existing
postconditions pass. The DS equations and operation ordering are unchanged.

`MultiCellPack::update_all_cells` checks the complete sensor set, finite current,
valid interval, nonempty cell set, and the consumed voltage-range denominator.
It computes cells, weak flags, and pack statistics in candidate storage, verifies
finite aggregate statistics, then commits using a nonthrowing vector swap and
scalar assignments. Cell success alone is insufficient: finite cell temperatures
can still overflow the aggregate sum or spread. Allocation failure during
candidate construction also leaves committed state untouched.

## Short-step contract and compatibility

For `DSCoupling::update`, `DSEnhancement::enhance`, and
`MultiCellPack::update_all_cells`, `0 < dt < tau_min` is rejected before mutation.
No elapsed time or readings are queued or accumulated by these APIs. Equality is
accepted. Callers must supply an interval at least as large as the configured
minimum or configure a smaller supported `tau_min`. MultiCellPack retains its
existing default coupling configuration (`tau_min = 0.01 s`); this phase does
not introduce pack configuration precedence changes.

Rejection was selected instead of silent deferral because the existing return
interface does not distinguish deferred evidence, and middleware would otherwise
advance diagnostic accounting while the adapter could authorize an earlier model
as the latest cycle. Existing examples use `0.1 s`, `0.01 s`, or larger intervals.
The torque/regen manager tests using `0.001 s` operate on already constructed
model snapshots and do not require sub-minimum core updates.

The existing middleware propagates the short-cycle exception before model/ledger
updates. The existing BMS adapter invalidates current-cycle evidence before the
call, so a rejected short cycle emits Level 3 / zero trust until recovery.
Optional FEEN result preparation in enhance now precedes the core state commit;
a propagated history-allocation failure cannot leave the core epoch advanced.
FEEN's own fallback/trust policies are unchanged.

Public signatures, C++17, accepted-input DS mathematics, current/temperature/SOC
normalization, chemistry policy, and prediction policy are preserved. New const
pack getters expose cached current and voltage extrema for complete verification.
Invalid pack sensor inputs now reject with `std::invalid_argument`; numeric
candidate failures retain `std::runtime_error`. Short calls that formerly returned
silently now throw. Successful pack commits invalidate references/iterators to
individual cell elements; the reference to the vector object returned by
`get_cells()` remains valid. Rejected cycles preserve element storage. Pack
transactions use O(number of cells) temporary storage and an allocation/copy per
accepted call; no realtime performance claim is made.

## Regression evidence

Failing tests were written and executed before each correction:

- PB-02: 341 checks, 15 failures before candidate-state computation; then passed.
- PB-01 after PB-02 alone: 3,772 checks, 806 failures before pack transaction
  correction, including current and voltage-cache getters. An initial overflow
  test incorrectly assumed `DBL_MAX A * 1 s` overflowed throughput. The stimulus
  was corrected to `DBL_MAX A * 10 s`; the same rejection/preservation assertions
  remain, and the representable one-second case was not prohibited by a new cap.
- PB-03 after the two atomicity fixes: 1,193 checks, 146 failures before minimum
  interval rejection, including middleware and adapter consequences.

Final `test_state_integrity` executes 5,681 checks: coupling 379, pack 4,029,
short-step 1,273. It compares all 17 DSState scalars, four gradients, and 16 metric
entries; scalar object-representation comparisons avoid struct padding and
preserve NaN/sign bits, with an explicit signed-zero coupling rejection case. Pack checks include every CellState field, all nine
cached statistics, weak IDs, health prediction, and recovery equivalence.
Cases cover invalid first/middle/last voltage and temperature, nonfinite current,
invalid timestep, finite cell/current/timestep overflow, aggregate temperature
sum/spread overflow, mismatched vector size, and late-cycle numerical rejection.
Short-step tests cover first-call rejection, changed voltage/SOC/current/temperature,
24 repeated short calls, the immediately adjacent floating-point intervals below
and above tau_min, exact equality, a smaller configured minimum with rejection/recovery, energy equations,
unchanged residual history, supported-step recovery, middleware, and adapter.

The final suite replayed against baseline headers fails (991 failed checks,
exit 1). The baseline replay adds only the three read-only pack getters to the
external archived header to allow the same tests to inspect private caches;
update logic is unchanged. One baseline recovery throws after its corrupted
state is reused, recorded as an unexpected-exception failure instead of aborting
or suppressing later groups.

## Validation and reproducibility

Cloud toolchain: GCC 14.2.0, Python 3.12.14, CMake 3.31.10 on Linux.
Build parallelism: four jobs. FEEN, embedded Python governor, and GPU disabled.
CMake defaults include examples, benchmarks, and all C++ tests.

- Release: complete build passes; CTest **13/13 passed**; assertions remain enabled
  by the existing `-UNDEBUG` test configuration; basic integration simulation passes.
- Debug: complete build passes with AddressSanitizer and UndefinedBehaviorSanitizer;
  CTest **13/13 passed**, leak detection and halt-on-error enabled, no sanitizer
  diagnostics. The new regression also passes all 5,681 checks in both builds.
- Python: discovery finds 20 tests. **17 passed, 1 skipped, 2 unrun**, zero failures
  or errors among the 18 selected tests. `test_limp_and_derate_modes` skips because
  optional pybind11 is disabled. `test_installer_safety_gating` and
  `test_backup_and_rollback_flow` are deliberately unrun: they invoke sudo,
  installation and rollback under `/opt`, prohibited by this mission. Their
  assertions and sources are unchanged; no dummy deployment was substituted.
  Discovery and the original hardened-suite `setUpClass` ran in a disposable
  source copy so its default rebuild could not overwrite the tracked CLI.
- Previous BEDROCK C++ tests remain registered and pass, including raw evidence,
  protection envelopes, direct numeric boundaries, regen boundaries, governor,
  and adapter availability. Historical release reports are unchanged.

Exact CTest targets, each passed in Release and Debug with ASan/UBSan:

```text
test_core                     test_state_integrity
test_feen_integration          test_advanced
test_middleware               test_protection_envelopes
test_direct_numeric_boundaries test_regen_boundaries
test_bms_adapter_evidence      test_ailee_governor
test_raps_ev_stability         test_ailee_trust_integration
test_cli_version
```

Python passing cases (`TestAileeAutomotiveDomain` in `test_ailee_automotive`):
`test_audit_logging`, `test_boolean_domain_decision_numbers_rejected`,
`test_domain_zero_authorization_is_preserved`, `test_governance_normal_mode`,
`test_governance_sensor_fault_fallback`, `test_governance_thermal_derating`,
`test_hp_calculations`, `test_invalid_or_overflowing_orchestrator_evidence_rejected`,
`test_malformed_domain_decision_rejected`,
`test_missing_or_malformed_direct_evidence_rejected`,
`test_valid_critical_policy_preserved`.

Python passing cases (`TestHardenedPipeline` in `test_hardened_pipeline`):
`test_diagnostics_help`, `test_non_finite_measurement_is_rejected`,
`test_soc_safety_boundaries`, `test_soh_safety_boundaries`,
`test_temperature_safety_boundaries`, `test_voltage_out_of_range`.

The external Python runner used the following selection after discovering the
untouched suite in a disposable source copy. Its class setup still runs:

```python
import sys, unittest
sys.path.insert(0, ".")
def flatten(suite):
    for item in suite:
        if isinstance(item, unittest.TestSuite):
            yield from flatten(item)
        else:
            yield item
excluded = {
    "test_hardened_pipeline.TestHardenedPipeline.test_installer_safety_gating",
    "test_hardened_pipeline.TestHardenedPipeline.test_backup_and_rollback_flow",
}
tests = list(flatten(unittest.defaultTestLoader.discover("tests", pattern="test_*.py")))
assert excluded <= {t.id() for t in tests}
result = unittest.TextTestRunner(verbosity=2).run(
    unittest.TestSuite(t for t in tests if t.id() not in excluded))
sys.exit(0 if result.wasSuccessful() else 1)
```

Build into separate directories. The onboarding output override
`/workspace/ds-setup/outputs.cmake` redirects ds_enhancer CLI/shared-library outputs
outside the checkout; otherwise the repository CMake targets overwrite its
tracked CLI binary. Commands used:

```sh
export PATH="/workspace/ds-tools/bin:$PATH"
cmake -S . -B /workspace/ds-build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PROJECT_INCLUDE=/workspace/ds-setup/outputs.cmake \
  -DDS_ENABLE_FEEN=OFF -DDS_ENABLE_PYTHON_GOVERNOR=OFF -DDS_USE_GPU=OFF
cmake --build /workspace/ds-build --parallel 4
ctest --test-dir /workspace/ds-build --output-on-failure
cmake -S . -B /workspace/ds-build-debug -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PROJECT_INCLUDE=/workspace/ds-setup/outputs.cmake \
  -DDS_ENABLE_FEEN=OFF -DDS_ENABLE_PYTHON_GOVERNOR=OFF -DDS_USE_GPU=OFF
cmake --build /workspace/ds-build-debug --parallel 4
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir /workspace/ds-build-debug --output-on-failure
```

The compatibility source `tests/state_integrity_valid_input_probe.cpp` prints
exact hexadecimal values for 15 direct coupling cycles with existing history,
six enhancement cycles including normalization, and four cycles each for 2- and
96-cell NMC packs. Compile the same source against the archived main headers and
patched headers with `c++ -std=c++17 -O3 -Wall -Wextra -Wpedantic -Werror -I<headers>`.
All 883 output lines compare byte-for-byte equal. Both output files have SHA256
`41f070fadb3556df94cf5310b46be6245df55c6fb5f7cdf710e66a98eb2181cd`.
This supports compatibility for those representative inputs, not universal binary
or numerical equivalence.

## Fresh adversarial review

A separate reviewer read the complete production, test, and documentation diff.
No additional production defect was found. Documentation typos and regression
coverage gaps were corrected: accepted voltage-extrema oracles, scalar-bit pack
cache comparisons, explicit short-cycle exception types, signed-zero rejection,
representable extreme-current acceptance, and custom-minimum recovery.
Independent external probes verified eight repeated FEEN-fallback history
allocation failures preserve the owned core state/residual and recover identically;
first/second candidate-pack allocation failures preserve cells, all nine caches
and element storage; malformed chemistry spans, empty cycles, and aggregate
voltage overflow reject without publication. The GPU conditional CPU stub was
compiled and exercised without CUDA; this is not GPU hardware validation.

## Scope and remaining limits

Only PB-01, PB-02 and PB-03 are resolved here. Other backlog findings remain open.
No hardware access, actuation, deployment, release publication, merge, calibration,
or safety certification was performed. Optional FEEN upstream integration,
embedded Python, CUDA, Valgrind, and hardware-specific workflows are not validated.
Software state transactions require the existing single-owner/caller synchronization
contract; this change does not provide concurrent reader atomicity.
