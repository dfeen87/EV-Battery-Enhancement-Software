# DS-EV 8.0.1 post-BEDROCK hardening report

## Scope, baseline, and compatibility

The reviewed baseline is v8.0.0 at
`d6748b9fc6d5036b357348f8555b3acbbcef1f10`. The working tree was clean before
investigation. [The BEDROCK report](RELEASE_8.0.0.md) and historical simulation
observations remain unchanged. The initial pass selects three demonstrated defects
in executed software boundaries. The follow-up on PR #38 builds on reviewed head
`b0558bcbb2157a129a4218d2d5f79904a0ed08d2` and addresses PB-04, PB-05, and PB-07;
remaining demonstrated findings are recorded in
[the remediation backlog](POST_BEDROCK_REMEDIATION_BACKLOG.md).

**SemVer: PATCH, 8.0.1.** Public C++ call signatures and result layouts remain compatible.
DS mathematics, configured four-level percentage envelopes, subsystem ownership,
and ordinary upward slew limits remain intact. Invalid or incomplete governance
evidence now produces finite zero authority, consistent with the existing
fail-closed contract. Valid protective evidence retains its configured derate;
those percentages are not a physical safety guarantee. A decreasing protection
ceiling takes precedence over downward slew smoothing.

CMake/CPack, API/CLI, README, installer metadata, current architecture version
markers, and update fallback references identify 8.0.1. No tag, release payload,
package publication, deployment, or merge is performed by this change. The updated
fallback URL requires a separately reviewed 8.0.1 payload publication before use.

## Implementation verified against documentation

The executable and C ABI in `ds_core/src/ds_enhancer.cpp` use
`include/ds_bms_middleware_v2.hpp`; the similarly named legacy middleware header
is a separate implementation, not an additional stage of that path.
`DSEnhancement` and `DSCoupling` live in `include/ds_battery_core.hpp`;
`ds_battery_enhancement.hpp` forwards the API. The non-governed drive limit manager
is in `include/torque_enhancement.hpp`, while the AILEE-managed command path is
`src/ds_torque_manager.cpp` and `src/ailee_horsepower_governor.cpp`.

BMS and anomaly compartments, TrustGate, and LearningEngine are implemented in
headers under `include/ailee_trust_layer/`. Their `src/ailee_trust_layer/*.cpp`
files are include anchors, not separate algorithms. LearningEngine is currently
exercised by reference tests; no production caller was found. Regenerative braking
and recovery coordination are header implementations. The reference hardware
adapter is `src/ds_bms_hardware_adapter.hpp`, not under `include/`.

Thermal, charging, and degradation compartments, the inter-compartment lock-free
bus, and fleet parameter distribution remain proposed in
`AILEE_EV_INTEGRATION_ARCHITECTURE.md`. They are not treated as implemented safety
controls. This review makes no binary/API equivalence claim with an upstream
AILEE artifact; the local Trust Contract 9.4 target remains distinct.

## Initial three selected defects and regression evidence

Selection prioritizes fault significance, deterministic reproduction, and actual
execution paths over unconnected reference features. Each regression was added
and executed against the unfixed implementation before correction.

| Rank | File/function and violated invariant | Before | Correction and after |
|---|---|---|---|
| 1, High | `DSBMSMiddleware::enhance_cycle` / `update_diagnostics` in `ds_bms_middleware_v2.hpp`: normalization must not erase safety evidence | `360 V, 10 A, 100 C, SOC .8, dt 1`: model temperature clamps to 60 C, but safety fault is false and BMS adapter reports nearly full trust / Level 0. Raw discharge 2000 A is likewise hidden by the 500 A model clamp. Infinite SOC is pre-clamped into valid evidence. | Preserve original physical readings for the existing safety monitor; pass original SOC through core finite validation. Normalized model state stays unchanged. Tests now observe thermal/current faults, adapter trust 0 / Level 3, and atomic rejection of NaN/Inf SOC. |
| 2, High | `DSTorqueManager::compute_torque_limit` and `DSRegenBrakingManager::compute_regen_limit`: emitted torque must not exceed the current protection ceiling | Regen after positive output, then full SOC and `dt .001`: 138.75 Nm despite zero acceptance. Drive healthy 340 Nm, then critical SOC: 337 Nm despite an 85 Nm configured ceiling. Hard-stop recovery can retain old torque history. | Project slew output onto the current target; remember emitted zero after a drive hard stop. Regen becomes zero at full SOC; drive becomes 85 Nm, recovering by its original 2 Nm/cycle rise. Sixteen previously failing checks pass, covering ABS/slip, voltage, thermal, pedal, speed, SOC, and recovery. |
| 3, High | `AileeHorsepowerGovernor::evaluate` and Python automotive pipeline/domain: malformed/missing evidence and non-finite derived power must not authorize output | C++ sensor-invalid input still authorizes 100 Nm; finite `DBL_MAX` torque/RPM/current produces infinite HP, NaN consistency, Level 0 / trust 1. Python empty evidence produces Level 0 / 400 Nm / 500 A; malformed fallback can throw while reparsing evidence. | Validate required evidence, domains, derived power, and returned decision numerics. Invalid evidence yields Level 3 / trust 0 / finite zero HP, torque, and current. Valid fault percentages remain unchanged. Manager rejection refreshes its last decision; Python audit fields for rejected numeric input remain finite. |

Middleware regression tests failed first at the raw-temperature safety assertion;
a separate infinite-SOC regression failed at the rejection assertion. The new
protection suite failed 16 checks before correction. C++ governor regressions failed
at nonzero sensor-fault authority; Python regression subcases demonstrated missing,
malformed, non-finite, out-of-domain, and overflowed evidence before correction.

The existing RAPS test fixture also used an uninitialized `EnhancedState` member,
preventing a strict GCC 14 Release build. Value-initializing the fixture restores
the build without changing assertions or suppressing compiler diagnostics.

## Initial validation and reproducible commands

Use C++17, GCC 14.2, CMake 3.31.10, and Python 3.12.14. CI defaults disable
FEEN, GPU, and the optional embedded Python governor. C++ test targets retain
assertions in Release. The new `test_protection_envelopes` target is registered
with CTest and exercises invariants directly rather than relying on `NDEBUG`.

The repository's CMake writes its CLI into the tracked `ds_core/bin/ds_enhancer`.
To preserve that historical file during validation, create an external include:

```cmake
function(review_output_paths)
  set_target_properties(ds_enhancer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
  set_target_properties(ds_enhancer_shared PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
  if(TARGET ds_enhancer_pybind)
    set_target_properties(ds_enhancer_pybind PROPERTIES
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
  endif()
endfunction()
cmake_language(DEFER CALL review_output_paths)
```

Then, from the checkout, use an external build directory:

```sh
cmake -S . -B /tmp/ds-release -DCMAKE_BUILD_TYPE=Release \
  -DDS_ENABLE_PYTHON_GOVERNOR=OFF \
  -DCMAKE_PROJECT_DS_Battery_Enhancement_INCLUDE=/tmp/review-output-paths.cmake
cmake --build /tmp/ds-release --parallel 4
ctest --test-dir /tmp/ds-release --output-on-failure
/tmp/ds-release/basic_integration
```

Repeat with a distinct Debug directory for ASan/UBSan. Use a temporary source copy
for Python discovery: imports and diagnostics append tracked audit/install logs,
and the hardened suite also performs its own build. Do not weaken those tests to
hide deployment prerequisites. Its installer/rollback tests call `sudo` and target
`/opt/ds_enhancement`; they are unsuitable for unapproved real-system deployment.

The final implementation was independently reviewed after both repair iterations.
Iteration 2 reproduced and corrected boolean-valued decision fields accepted as
numbers in the Python pipeline and optional C++ bridge. All numeric bridge fields
are covered by injected malformed-result tests. An explicit domain denial also
remains zero rather than being reopened by percentage fallback. No unresolved
regression was found within the selected corrections.

| Check | BEDROCK baseline | Final outcome |
|---|---|---|
| Strict C++ Release build, all default targets | Failed on uninitialized RAPS test fixture | Passed; all examples, tests, benchmarks, CLI and shared library built |
| All registered default Release CTest tests | Not completed because build failed | **9/9 passed**, assertions retained |
| Debug build and all registered CTest tests, ASan/UBSan | **8/8 passed** | **9/9 passed**, no sanitizer diagnostics |
| `basic_integration` simulation | Debug simulation passed | Release and Debug simulations passed |
| Full Python discovery / CI Python command | 14 tests: **11 passed, 2 errors, 1 skipped** | 20 tests: **18 passed, 2 errors, 0 skipped** |
| Optional embedded Python governor / trust integration | Not run | Both selected CTest tests passed (**2/2**); malformed-result injection executed |
| Optional pybind module | Not built; binding test skipped | Built; existing limp/thermal/weak-cell Python test passed |
| Python source compilation | Not recorded | 11 main-interface, test and script files compiled syntactically |
| CLI, ctypes C ABI, REST examples | Established during environment onboarding | CLI 8.0.1 JSON and invalid-SOC exit, ctypes rejected-cycle equivalence, REST health and six populated JSON endpoints passed |
| Repository cppcheck gate / `make verify` | Nonzero: 52 uninitialized-member warnings, 4 performance findings, 1 `throwInEntryPoint` error | Same finding IDs and counts; `make verify` still returns nonzero |
| Repository clang-tidy invocation | Exit 0 | Exit 0; default tool output reports one suppressed non-user-code warning |
| Targeted clang-tidy for changed governor/command source | Not recorded | Exit 0 using the Release compilation database |
| Hosted GitHub CI | Not rerun on unchanged baseline | Push and PR workflows reported success for `62202ed69adcf76202808525a56bbc604967fa96` |
| Diff / historical artifacts | Clean initial tree | Whitespace check passed; BEDROCK report, historical simulations and tracked CLI binary unchanged |

**Hosted CI and cloud-local validation differ.** Full cloud-local Python discovery errors in
`test_installer_safety_gating` and `test_backup_and_rollback_flow` because `sudo`
is unavailable. Their deployment actions did not execute. The other 18 tests,
including the newly available binding test, completed successfully. The cppcheck
gate still reports the baseline `DS_EXAMPLE_MAIN` entry-point exception finding
at `ds_battery_core.hpp:644` plus baseline aggregate-initialization and performance
findings; no finding was suppressed to obtain a pass. `make verify` stops there,
before its host checks. The successful strict builds independently verify the
compiler and platform prerequisites.

GitHub reports successful [push CI](https://github.com/dfeen87/DS-EV-Battery-Enhancement-Software/actions/runs/38039909719)
and [PR CI](https://github.com/dfeen87/DS-EV-Battery-Enhancement-Software/actions/runs/38039946242)
for the implementation commit above. The hosted job reports successful CMake
build, CTest, Python discovery, and simulation steps. Its status is distinct from
the two missing-`sudo` errors in this cloud machine and from the separate
`make verify` static gate. Hosted per-test counts are not claimed here: workflow
logs were unavailable through the current log-download network route. Hosted
installer/rollback checks are software tests on an ephemeral CI runner, not a
safety-critical deployment or physical-system validation.

The optional build uses the repository's pybind11 v2.12.0 pin. This cloud Python's
sysconfig points at `/install/lib`, while its actual library lives under its
runtime prefix. Setting `PYTHON_LIBRARY` to the installed `libpython3.12.so`
resolved linking without source changes or disabling verification. Reproduce the
optional checks with `DS_ENABLE_PYTHON_GOVERNOR=ON`, that library path when needed,
and `PYTHONPATH=<temporary-source-copy>/ds_core/python` so the tests exercise the
real embedded module. They ran with bytecode disabled and audit output confined to
the temporary copy. FEEN/GPU and unrelated hardware paths were not enabled.

## PR #38 protection-boundary follow-up

The follow-up starts from a clean tree at `b0558bc`, identical to the open draft
PR's head. Before edits, all default targets rebuild with strict C++17 Release
warnings, all 9 registered CTest tests pass, and all 11 automotive Python tests
pass. The three selected backlog reproductions remain present at that baseline.
Existing passing tests and the initial v8.0.1 corrections are retained.

| Finding | Before | After and regression |
|---|---|---|
| PB-04, direct invalid numeric evidence | NaN temperature: RAPS reports no trip/stability 1; direct drive allows 340 Nm; regen allows 143.75 Nm with temperature factor 1. | RAPS reports an invalid-input trip/stability 0; both managers emit 0 Nm with an explicit denial reason. `test_direct_numeric_boundaries` and `test_regen_boundaries` cover nonfinite consumed inputs, domains, positive dt, malformed configurations, derived overflow, and accepted history after rejection. |
| PB-05, power ceiling | `max_regen_power_kw=1`, 30000 RPM: 143.75 Nm represents 451.604 kW. | 0.183028 Nm represents 0.575 kW, below the configured 1 kW ceiling after the existing RAPS factor. Zero/near-zero/extreme speed and immediate tightening/recovery are covered by `test_regen_boundaries`. |
| PB-05, voltage precedence | Configured 410 V maximum, measured 409 V: 0 Nm without diagnostics, 143.75 Nm with diagnostics. | 0 Nm in both paths. Configured taper/hard-stop boundaries and a sub-1 V voltage-policy bypass have regression coverage. |
| PB-07, adapter evidence | Construction/initialization and a rejected latest cycle allow trust 1 / Level 0; TrustGate permits learning. | Missing, reset, or rejected latest-attempt evidence gives health/trust 0, anomaly true, Level 3, and no learning authorization. A genuine successful update restores the existing healthy or physical-fault policy. `test_bms_adapter_evidence` covers these transitions and unchanged-model recovery. |
| PB-04 direct dependency, governed RAPS rejection | After RAPS numerical hardening, a finite `-DBL_MAX` temperature overflows its derivative: applied torque is 0, but output trust is 1 and latest governor audit is Level 0 / trust 1. | `DSAileeTorqueManager::processTorqueCommand` treats RAPS input/numerical rejection as unavailable authorization evidence before governor evaluation. Output and latest audit both become Level 3 / trust 0 / finite zero authority. `test_ailee_governor` verifies rejection and ordinary recovery. Physical RAPS fault policy is retained. |

Changes are confined to `RapsEVStabilityMembrane::evaluate`/initialization in
`include/raps_ev_stability_membrane.hpp`, direct `DSTorqueManager` validation and
candidate computation (including its consumed `ThermalModel::update`) in
`include/torque_enhancement.hpp`, `DSRegenBrakingManager` in
`include/ds_regen_braking_manager_v1.hpp`, `BMSMiddlewareAdapter` in
`include/ailee_trust_layer/ailee_adapters.hpp`, and the governed wrapper above.
The three new CTest targets are registered in CMake. Public call signatures,
result layouts, valid governance percentages, and DS equations are unchanged.
Malformed configurations now throw `std::invalid_argument` before initialization
mutates active state. RAPS adds diagnostic `INVALID_INPUT` and
`NUMERICAL_FAILURE` enum values after the existing values. Private header-only
manager/adapter storage changes require consumers to rebuild; this is source
compatibility, not an independent binary compatibility claim.

Rejected numeric candidates preserve accepted filter, thermal, timer, and ABS
recovery history. The emitted command history records zero so recovery ramps from
the denied limit. Regen publishes rejection diagnostics separately from its
accepted stability filter. Tighter power, voltage, battery-acceptance, and pedal
ceilings remain immediate; upward slew/recovery policies remain intact. A
600-cycle differential comparison of valid direct drive inputs (RAPS enabled and
disabled, temperatures -10..59 C, SOC 0..1, RPM 0..16000, signed current, and three
positive timesteps) produces identical numeric outputs, thermal state, and
diagnostics before and after.

The power calculation is mechanical: `P_kW = T_Nm * RPM * 2*pi/60000`, equivalent
to `T_Nm * omega_rad_per_s / 1000`. It does not assume electrical recovery
efficiency. Zero RPM preserves the documented optional-speed behavior, avoids
division, and retains the other torque/acceptance ceilings; zero configured power
disables regen even then. Actual positive-speed enforcement requires a truthful
RPM input. `pack_voltage_max_fallback_v` keeps its compatible name but is always
authoritative because `DiagnosticReport` carries no replacement maximum. A
diagnostic safety fault can tighten protection, never relax this ceiling.

Adapter availability records completion of the latest observed middleware call.
It clears before each attempt and on initialization/reset; a thrown update leaves
it unavailable while preserving middleware last-good state. It does not prove
external sensor freshness or a new model epoch. Repeated evaluation does not
invent an update. PB-03's short-timestep model inconsistency remains open.

### Independent review and the single corrective iteration

The complete PR diff against BEDROCK was independently inspected after the initial
follow-up repairs. Two additional numeric cases were demonstrated, regressed
before correction, and resolved together in one focused corrective iteration:

- With `max_regen_power_kw=2*denorm_min` (`9.88131e-324 kW`) and RPM `7e-320`,
  a positive subnormal conversion factor allowed about `2 Nm`, representing
  `1.48220e-323 kW` after 1000 accepted cycles. A zero-conversion variant at RPM
  `1e-320` likewise bypassed the cap. Positive RPM whose kW/Nm conversion is zero
  or subnormal now gives `NUMERICAL_FAILURE`, torque/power `0`, and unchanged
  accepted history. Explicit RPM `0` retains its separate compatible contract.
  The new regression first failed 10 checks, then passed.
- Finite `DBL_MAX` requested torque at RPM `1`, with matching finite electrical
  horsepower and warmed RAPS history, reached a `1.2` boost and published an
  infinite authorized torque ceiling at Level 0 / trust 1. Governed boost,
  HP-to-torque, and applied-HP arithmetic are now checked; rejected candidates
  return Level 3 / trust 0 / finite zero authority and zero audit limits. RAPS
  history commits only after numerically valid output or a valid physical DSM
  decision. The new governor regression first failed its protective-level
  assertion, then passed, including recovery against an unchanged reference.

The independent reviewer rebuilt and reran all three adversarial reproductions
and both complete focused suites against the final code. Each reproduction now
emits finite zero authority; no unresolved regression was identified in the
selected boundaries. The direct numerical suite's final source has 285 failing
checks against reviewed baseline headers and 295 passing checks after repair;
the adapter suite has 42 failures against baseline and passes after repair.

### Follow-up validation results

All outcomes below refer to the final follow-up implementation, after that
corrective iteration. No test assertions, warnings, or CI gates were weakened.

| Check | Outcome |
|---|---|
| Strict C++17 Release, all default targets | Passed; **12/12 CTest tests passed** |
| Debug ASan/UBSan, all default targets | Passed; **12/12 CTest tests passed**, no sanitizer diagnostics |
| Optional embedded Python governor, all configured targets and pybind | Passed; **12/12 CTest tests passed**, including malformed-result injection and governed numerical rejection |
| Full Python discovery in temporary source copy | **18 passed, 2 errors, 0 skipped**; the two installer/rollback tests fail because `sudo` is unavailable, before deployment executes |
| Automotive Python regressions | All **11 passed** within full discovery |
| CLI / ctypes | Version 8.0.1, finite normal JSON, invalid SOC/dt rejection, and ctypes rejected-cycle recovery equivalence passed |
| Simulations / REST example | Release and Debug `basic_integration`, Release RAPS demo, REST health and six populated finite JSON endpoints passed |
| Python syntax checks | Main interfaces, tests, and Python scripts passed |
| `make verify` / cppcheck | Nonzero, unchanged baseline: 52 `uninitMemberVarNoCtor`, 4 `returnByReference`, 1 `throwInEntryPoint`; host checks are not reached |
| Repository and targeted clang-tidy | Both exit 0. Repository invocation reports one default-suppressed non-user-code warning; targeted governed source and three new C++ regression files report five such warnings using the Release compilation database |
| Hosted GitHub CI for follow-up implementation `6e52bfd52062a08f820356f38369291d73f4c248` | Push and PR workflows succeed; CMake build, CTest, Python discovery, and simulation steps report success |
| Final diff / historical artifacts | Whitespace check passed; v8.0.0 release report, historical simulation observations, and tracked BEDROCK CLI binary unchanged |

Hosted [push CI](https://github.com/dfeen87/DS-EV-Battery-Enhancement-Software/actions/runs/38043147399)
and [PR CI](https://github.com/dfeen87/DS-EV-Battery-Enhancement-Software/actions/runs/38043151694)
validate the follow-up implementation commit above. Per-step results were verified
through the GitHub API; hosted per-test counts are not claimed. These successes
remain distinct from the cloud-local missing-`sudo` errors and separate cppcheck
gate. Hosted annotations also report the checkout action's Node.js runtime
deprecation and the planned `ubuntu-latest` image migration; those workflows still
complete successfully. The existing PR remains a draft for human review.

**Version remains PATCH 8.0.1:** these changes enforce existing finite-evidence,
fail-closed, configured-ceiling, and current-cycle authorization contracts.
They introduce no new operating feature or breaking public call signature.
Historical v8.0.0 evidence remains unchanged; no release, binary artifact, or
deployment is published.

## Assurance boundaries

This work validates software behavior and repository simulations. It does not
establish an independent SIL integration, HIL behavior, battery-pack performance,
vehicle calibration, road behavior, or automotive certification. No hardware
commands, real charging, road tests, or safety-critical deployment were performed.

The BMS fix exposes raw faults to diagnostics and its adapter. DS charging profiles
remain model recommendations; the C ABI/CLI do not expose a control-grade fault
field. A caller must not interpret numerical stability or a positive recommendation
as actuation permission. The remaining evidence, configuration, transaction, and
provenance defects in the backlog prevent any defect-free or physical-safety claim.
