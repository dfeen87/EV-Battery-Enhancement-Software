# Post-BEDROCK remediation backlog

This backlog records findings demonstrated against the v8.0.0 BEDROCK
implementation and the reviewed v8.0.1 PR baseline. It is an engineering work queue,
not a claim of automotive certification or a complete hazard analysis.

The initial three repair workstreams are raw BMS fault preservation, current
drive/regen ceilings taking precedence over transition smoothing, and malformed
C++/Python governor evidence. The review is limited to two focused review-and-repair
iterations. The follow-up on the same PR addresses PB-04, PB-05, and PB-07 with
one independent review and at most one focused corrective iteration. Resolved
entries retain their original reproduction evidence; tests on other entries remain
requirements for subsequent work, not reported passing tests.

Severity describes the software boundary and potential consequence. Physical
consequences have not been verified. Reproductions use synthetic in-process values
and, where applicable, a fake CAN transport; no hardware commands are needed.
Use C++17 with the repository headers for C++ reproductions. `NaN`, `DBL_MAX`, and
`inf` mean quiet NaN, the largest finite double, and positive infinity respectively.

## Implementation order

1. PB-01 through PB-08: preserve coherent state, reject malformed direct-control
   evidence, and honor configured protection limits and evidence availability.
2. PB-09 through PB-12: repair recovery accounting, learning authorization, and
   observable audit consistency. Learning fixes become prerequisites before wiring
   that reference engine into a production control loop.
3. PB-13 through PB-15: reconcile middleware accounting/configuration and validate
   the separate Python fallback model.

Dependencies below are technical ordering suggestions; unrelated items can proceed
independently. Keep public interfaces, valid configured policies, and DS mathematics
compatible where possible. Add failing regressions before each correction.

## PB-01 — Partial multi-cell commit after a rejected cycle

- **Severity / confidence:** High; confirmed.
- **File / function:** `include/ds_advanced_features.hpp`, `ds::advanced::MultiCellPack::update_all_cells`.
- **Reproduction:** Construct an NMC two-cell pack. Update voltages `{3.7,3.7}`, temperatures `{25,25}`, current `10 A`, `dt=1 s`. Then update `{3.8,NaN}`, `{30,30}`, `20 A`, `dt=1 s`. The second call throws `Invalid DSState before update`; cell times are `2 s` and `1 s`, cell 1 voltage is NaN, and cached pack voltage remains `7.4 V`.
- **Impact / invariant:** Failure publishes raw invalid fields and different cell epochs while pack statistics describe the earlier cycle. A failed pack update must preserve all previously committed cells, statistics, and current.
- **Minimal correction:** Validate the complete input set, compute all cells and pack statistics in candidate storage, and commit them together only after every cell succeeds.
- **Required regressions:** Invalid first/middle/last cell, invalid current/timestep, and finite overflow; compare every cell and statistic before/after failure; preserve ordinary pack updates.
- **Dependencies:** PB-02 improves the lower-level update boundary but does not by itself make a multi-cell transaction atomic.

## PB-02 — Direct DS coupling mutates state before numerical failure

- **Severity / confidence:** High; confirmed for direct callers.
- **File / function:** `include/ds_battery_core.hpp`, `ds::DSCoupling::update`.
- **Reproduction:** Start a valid state at `400 V`, `10 A`, SOC `0.8`, then `update(state,1)`. Call `update(state,DBL_MAX)`. It throws `Invalid DSState after update`, but time changes from `1` to approximately `1.79769e308`, charge throughput becomes `inf`, and `state.is_valid()` is false.
- **Impact / invariant:** A caught exception leaves the caller's previously valid state corrupted. `DSEnhancement::enhance` already protects its owned state with a candidate; direct coupling callers do not receive that protection.
- **Minimal correction:** Compute into a candidate inside `DSCoupling::update` and assign the supplied state only after postconditions pass.
- **Required regressions:** Direct finite-overflow rejection preserves the complete state; malformed input state and invalid timestep preserve it; normal updates retain existing numerical results.
- **Dependencies:** Implement before or alongside PB-01; preserve the outer enhancement candidate/commit contract.

## PB-03 — Sub-minimum timestep publishes mixed physical and computed state

- **Severity / confidence:** High; confirmed.
- **File / functions:** `include/ds_battery_core.hpp`, `ds::DSEnhancement::enhance` and the `dt < tau_min` return in `ds::DSCoupling::update`.
- **Reproduction:** Default configuration (`tau_min=0.01 s`, capacity `75 Ah`): `enhance(360,0,25,0.8,1)`, then `enhance(400,0,25,0.5,0.001)`. The second result publishes `400 V` and SOC `0.5`, time remains `1 s`, `energy_psi=7.776e7 J` although those physical fields require `5.4e7 J`, and `numerical_stability=true`.
- **Impact / invariant:** The energy-sum residual passes because all stored energy terms are old. One published snapshot must describe a coherent model epoch and physical inputs.
- **Minimal correction:** Define and enforce the short-step contract at the owning enhancement boundary: defer the entire cycle, reject it before mutation, or accumulate time/inputs for a full update. Do not silently commit new fields with stale derived values.
- **Required regressions:** Below, exactly at, and above `tau_min`; changed voltage/SOC; repeated short steps; state, time, energy, and stability consistency.
- **Dependencies:** Review existing simulation short-step expectations before choosing the compatible behavior; PB-02 alone does not resolve the early return.

## PB-04 — Direct RAPS, torque, and regen paths accept NaN temperature — resolved

- **Severity / confidence:** High; confirmed outside the repaired governor boundary.
- **File / functions:** `include/raps_ev_stability_membrane.hpp`, `raps::ev::RapsEVStabilityMembrane::evaluate`; `include/torque_enhancement.hpp`, `ds::drive::DSTorqueManager::compute_torque_limit`; `include/ds_regen_braking_manager_v1.hpp`, `ds::drive::DSRegenBrakingManager::compute_regen_limit`.
- **Reproduction:** `membrane.evaluate(400,0,NaN)` returns `dsm_tripped=false`, stability `1`. An otherwise healthy `EnhancedState` with SOC `0.5`, health `100%`, confidence `1`, voltage `400 V`, current `0 A`, and temperature NaN permits `340 Nm` through the default direct torque manager at `3000 RPM`, `dt=1 s`. Direct regen with brake `1`, speed `50 km/h`, `3000 RPM`, no ABS/slip/diagnostics, `dt=1 s` permits `143.75 Nm`, with temperature factor `1`.
- **Impact / invariant:** NaN bypasses ordered thermal comparisons and contaminates filter history while allowing nonzero control limits. Governor validation does not protect these separately callable managers.
- **Minimal correction:** Validate consumed sensor/state values and timestep before filter mutation; return a deterministic denied result for malformed evidence. Validate any configuration domains needed for those calculations without changing valid policies.
- **Required regressions:** NaN/infinity and extreme finite consumed fields on direct paths; zero authority and no history contamination after failure; normal thermal/current filtering and recovery.
- **Dependencies:** Preserve the repaired protection-ceiling/slew ordering; coordinate common RAPS handling with both direct managers.
- **8.0.1 correction / evidence:** Direct entry points validate consumed finite values, domains, positive timesteps, and configuration denominators before calculation. RAPS and drive compute candidates; regen stages RAPS/ABS history. Invalid or overflowing candidates emit finite zero authority with an explicit denial reason, retain accepted numeric filter/thermal/timer histories, and anchor command recovery at emitted zero. Returned diagnostics identify denial; regen also publishes it through its diagnostics getter. NaN temperature now produces a RAPS trip with stability `0`, drive `0 Nm`, and regen `0 Nm`. `test_direct_numeric_boundaries` and `test_regen_boundaries` cover NaN/Inf, finite overflow, malformed configuration, rejection history, and ordinary valid operation. The direct suite initially failed 136 checks and the regen suite 148 checks before correction. The governed wrapper's newly reachable RAPS numerical rejection also refreshes zero-trust/Level 3 audit evidence; `test_ailee_governor` first failed its zero-trust assertion and now passes.
- **Scope:** Existing valid physical derating policies remain configured software policies, not guarantees of physical safety. A manager cannot recover raw readings from a normalized `EnhancedState`; callers must provide original middleware diagnostics for raw faults. Independent hardware evidence and model-epoch consistency remain outside this correction.
- **Independent corrective iteration:** A finite `DBL_MAX` torque command with matching finite electrical HP and warmed RAPS filters could overflow the governed boost ceiling to `inf` at Level 0 / trust 1. A failing `test_ailee_governor` regression precedes guards for boost/HP-to-torque/applied-HP arithmetic and staged RAPS history. The final repro emits Level 3 / trust 0 / finite zero limits, and recovery matches an unchanged reference. The independent reviewer verified the fix. Final direct numerical regression source: 285 failures against baseline headers, 295 checks pass after repair.

## PB-05 — Regen ignores a configured power ceiling and replaces voltage policy — resolved

- **Severity / confidence:** High; confirmed.
- **File / functions:** `include/ds_regen_braking_manager_v1.hpp`, `ds::drive::DSRegenBrakingManager::compute_regen_limit` and `diag_pack_max_voltage`.
- **Reproduction:** With the healthy state from PB-04, configure `max_regen_power_kw=1`; brake `1`, speed `50 km/h`, motor `30000 RPM`, `dt=1 s` permits `143.75 Nm`, equivalent to `451.604 kW` mechanical power. Separately configure maximum fallback voltage `410 V` and measure `409 V`: the same request at `3000 RPM` yields `0 Nm` without diagnostics, but `143.75 Nm` with an otherwise ordinary diagnostic pointer. `diag_pack_max_voltage` hardcodes `450 V`.
- **Impact / invariant:** The declared power ceiling is unused, and merely providing diagnostics relaxes the selected pack-voltage boundary.
- **Minimal correction:** Apply the configured power ceiling using validated speed and explicit physical units; keep the configured voltage limit authoritative when diagnostics carry no replacement limit.
- **Required regressions:** Power caps across motor speeds including zero/near-zero speed; restrictive configured voltages with/without identical diagnostics; limits composed with SOC, thermal, ABS, RAPS, and slew protections.
- **Dependencies:** PB-04 input validation; preserve configured derating policies and the existing mechanical/electrical efficiency distinction.
- **8.0.1 correction / evidence:** `max_regen_power_kw` caps mechanical power using `P_kW = T_Nm * RPM * 2*pi/60000`; angular-speed conversion and conditional division avoid overflow and division by zero. The original `1 kW` / `30000 RPM` case now emits `0.183028 Nm`, or `0.575 kW` after existing RAPS derating. `pack_voltage_max_fallback_v` remains authoritative because diagnostics contain no alternative maximum. The `410 V` configured / `409 V` measured case emits `0 Nm` both with and without diagnostics. A sub-1 V configured voltage regression also prevents the former voltage-policy bypass. `test_regen_boundaries` covers zero/near-zero/extreme/nonfinite RPM, zero power, voltage equality/taper/hard stop, immediate power reductions, and rejection after accepted ABS history.
- **Scope:** Zero RPM retains the prior optional-speed contract: supplied angular frequency is zero, torque remains bounded by other ceilings, and no division occurs. Positive-speed power enforcement requires actual RPM from the caller; this software does not prove rotor speed or electrical recovery efficiency. No replacement voltage-policy field or external freshness guarantee is invented.
- **Independent corrective iteration:** RPM `7e-320`, power ceiling `2*denorm_min` (`9.88131e-324 kW`), and warmed filters emitted about `2 Nm` / `1.48220e-323 kW`; RPM `1e-320` also lost the conversion entirely. Zero or subnormal power-conversion factors for positive RPM now deny with finite zero authority without advancing accepted history. The new regression failed 10 checks before this correction and passes afterward; the independent reviewer verified both cases.

## PB-06 — Configured lower-temperature and SOC limits are unused

- **Severity / confidence:** High; confirmed configuration gap; directional policy requires clarification in implementation.
- **File / function:** `include/ds_bms_middleware_v2.hpp`, `ds_plugin::SafetyMonitor::check` and `SafetyLimits`.
- **Reproduction:** Configure `min_operating_temp=0 C`, `min_soc=0.2`, `max_soc=0.8`. Use a healthy `400 V`, `0 A` state and `max_cell_temp_c=25` diagnostics. Temperature `-5 C` with SOC `0.5`, then temperature `25 C` with SOC `0.1` and `0.9`, each returns `check=true`, `has_fault=false`.
- **Impact / invariant:** Publicly configured limits do not participate in monitoring. This demonstrates missing enforcement, not proof that charging/discharging require identical fault behavior.
- **Minimal correction:** Establish the intended charge/discharge meaning of these existing fields, then enforce the applicable conditions on raw evidence with explicit diagnostics; document any intentionally advisory field.
- **Required regressions:** Configured and default lower-temperature/SOC boundaries, current direction, zero current, and exact equality; verify applicable restrictions without changing normal behavior.
- **Dependencies:** Build on raw fault preservation; review consumers of `safety_fault` so advisory recommendations are not misrepresented as actuator shutdown.

## PB-07 — BMS adapter authorizes missing or rejected-cycle evidence — resolved at adapter boundary

- **Severity / confidence:** High; confirmed in the reference adapter.
- **File / functions:** `include/ailee_trust_layer/ailee_adapters.hpp`, `ailee::ev::adapters::BMSMiddlewareAdapter::initialize`, `update_cycle`, and `evaluate`.
- **Reproduction:** Initialize an adapter and call `evaluate(1)` before any BMS cycle. It returns trust `1`, Level 0. After a valid cycle, a malformed cycle that throws leaves valid earlier diagnostics available; `evaluate` has no indication that the latest attempted cycle failed.
- **Impact / invariant:** Default/stale diagnostics become current authorization evidence. This is a deterministic missing-evidence defect, distinct from unproven external sensor freshness.
- **Minimal correction:** Track successful-cycle availability and rejected-cycle status at the adapter boundary; return invalid/zero-trust protective evidence until a successful current cycle exists. Preserve the core's last-known-good state for inspection.
- **Required regressions:** Before initialization, after initialization without a cycle, successful cycle, rejected cycle, and subsequent successful recovery; verify trust-gate decisions and availability metadata.
- **Dependencies:** Raw fault preservation is necessary but does not supply cycle availability; avoid silently resetting useful diagnostic history.
- **8.0.1 correction / evidence:** A private availability flag starts false, clears on initialization/reset and before every attempted update, and becomes true only after `enhance_cycle` returns. Unavailable evaluation and the recommendation getter return Level 3; telemetry has health/trust `0`, anomaly true, and an explicit missing-successful-update reason. Middleware exceptions propagate and its last good model remains intact. `test_bms_adapter_evidence` has 42 failing checks against reviewed head `b0558bc`, then passes after correction, including missing evidence, rejected latest cycle, finite-overflow rejection, recovery, reset, and TrustGate learning denial.
- **Scope:** Availability proves only successful completion of the latest adapter-observed middleware call. Repeated evaluation is allowed until the next update attempt; no timestamp or external sensor-freshness claim is made. PB-03's sub-minimum timestep/model-epoch defect remains open and is not concealed by this flag.

## PB-08 — Hardware adapter treats future and NaN clock evidence as fresh

- **Severity / confidence:** High; confirmed with a fake transport only.
- **File / functions:** `src/ds_bms_hardware_adapter.hpp`, `ds_plugin::DSHardwareAdapter::set_now_seconds` and `get_fresh_or_throw`.
- **Reproduction:** Cache a synthetic pack-voltage CAN frame for `360 V` at timestamp `100 s` with the default maximum signal age `0.5 s`. Set now to `0 s`: `read_pack_voltage()` returns `360 V` because age is `-100`. Set now to NaN: it still returns `360 V` because `age > max_age` is false. The fake transport's send method throws and is never invoked.
- **Impact / invariant:** Clock rollback or malformed clocks defeat the adapter's explicit freshness check. A usable age must be finite, nonnegative, and within the configured limit.
- **Minimal correction:** Validate clock and age values and a finite nonnegative configured maximum age; reject future/malformed cache timing before returning the signal.
- **Required regressions:** Future timestamp, backward clock, NaN/infinite clocks and ages, exact maximum-age equality, ordinary fresh and expired signals.
- **Dependencies:** Independent software correction; physical clock synchronization, CAN authenticity, and real hardware behavior remain outside this reproduction.

## PB-09 — Recovery coordinator mutates rejected history and overcounts recovery

- **Severity / confidence:** Medium; confirmed.
- **File / functions:** `include/ds_energy_recovery_coordinator_v1.hpp`, `ds::drive::DSEnergyRecoveryCoordinator::step` and `smooth_current`.
- **Reproduction:** Set `prefer_measured_pack_current=false`. A `100 Nm` regen request at `400 V`, NaN temperature, SOC `0.5`, omega `100 rad/s`, `dt=0.1 s` is rejected by BMS after smoothing history changes. A subsequent valid zero-torque step uses `-4.94156 A`; a fresh coordinator uses `0 A`. Separately, `250 Nm`, omega `10000 rad/s`, `400 V`, `dt=1 s` records `0.590278 kWh`, while the smoothed BMS current `-249.989 A` represents `0.0277765 kWh`.
- **Impact / invariant:** A rejected cycle changes future behavior. Estimated unsmoothed mechanical power is recorded as recovered battery energy despite current limits and smoothing.
- **Minimal correction:** Validate before mutation and stage coordinator history/diagnostics until BMS success. Account battery-side recovery from the accepted current/voltage; expose estimated available mechanical energy separately if needed.
- **Required regressions:** Rejected-cycle state equivalence to a fresh/unchanged coordinator; measured/estimated modes, smoothing and current caps, unit/sign consistency, and no double counting.
- **Dependencies:** Keep BMS accepted-state accounting consistent with PB-13; estimates remain software quantities, not measured pack recovery.

## PB-10 — Learning rollback crosses targets and restores invalid baselines

- **Severity / confidence:** High; confirmed reference-engine defect; no production control-loop call sites found.
- **File / functions:** `include/ailee_trust_layer/learning_engine.hpp`, `ailee::ev::LearningEngine::propose_parameter_update` and `rollback`; `include/ailee_trust_layer/ailee_trust_gate.hpp`, `TrustGate::verify_parameter_update`.
- **Reproduction:** Approve `regen_torque_limit` from `100` to `115` under envelope `[50,300]`, step `20`, trust `0.92`. Roll its handle back against `fast_charge_c_rate` envelope `[0.5,3]`, current `1`: success sets the charging parameter to `100`. Separately, envelope `[0,1]`, current `1.01`, step `0.05`, proposed `1`, trust `0.95` is approved; rollback restores invalid `1.01`.
- **Impact / invariant:** A rollback handle does not authorize a different parameter or out-of-envelope state. Invalid pre-update state must not become a trusted restoration snapshot.
- **Minimal correction:** Validate the entire current envelope before proposals; bind snapshots to their target and validate restoration against the current envelope before mutation. The rollback API has no compartment argument: matching names alone cannot distinguish equal-named parameters in different compartments. Choose an explicit compatible target-binding/lifetime contract before connecting it to control loops.
- **Required regressions:** Foreign target, same name/different target, tightened envelope, malformed envelope, invalid prior value, rejected handle, ordinary rollback and rollback history; failure leaves target/history unchanged.
- **Dependencies:** PB-11 score/policy validation; preserve rollback aliases and public interface compatibility where feasible.

## PB-11 — Invalid trust configuration and out-of-domain scores authorize learning

- **Severity / confidence:** High; confirmed reference trust-gate defect; current call sites are integration tests.
- **File / functions:** `include/ailee_trust_layer/ailee_trust_gate.hpp`, `ailee::ev::TrustGate` constructor, `evaluate_telemetry`, `verify_parameter_update`, and `verify_protective_mode_exit`.
- **Reproduction:** Set accept, soft, and hard thresholds to NaN. A telemetry item with trust `0` returns Level 0 with learning allowed; a bounded parameter update at trust `0` is accepted. With default policy, trust `2` accepts a valid parameter update and protective-mode exit at `100` healthy cycles, although telemetry aggregation rejects scores above `1`.
- **Impact / invariant:** Malformed configured policy bypasses ordered comparisons; authorization entry points disagree on the documented score domain `[0,1]`.
- **Minimal correction:** Validate finite, ordered thresholds within `[0,1]`; invalid configuration must fail closed. Enforce the same score domain on learning and recovery while retaining valid configurable thresholds and recovery policies.
- **Required regressions:** NaN/infinite/out-of-range/unordered thresholds; scores below zero/above one/nonfinite at every gate; valid nondefault policy and exact threshold equality.
- **Dependencies:** Implement with or before PB-10; do not replace a valid configured threshold with a hardcoded default.

## PB-12 — Learning audit records wrong prior state and invalid JSON

- **Severity / confidence:** Medium; confirmed.
- **File / functions:** `include/ailee_trust_layer/learning_engine.hpp`, `ailee::ev::LearningEngine::rollback` and `write_audit_log`.
- **Reproduction:** Approve `0.5 -> 0.54 -> 0.58`, then roll back the first handle. It restores `0.5`, but the new snapshot reports previous value `0.54` rather than actual `0.58`. A rejected NaN proposal writes an unquoted `nan` numeric token; compartment name `charger"identity` is written without JSON escaping. Both latter lines fail `json.loads`.
- **Impact / invariant:** Snapshot history does not accurately describe the executed transition, and malformed evidence can break machine-readable audit ingestion.
- **Minimal correction:** Snapshot the actual target value before rollback; serialize escaped strings and explicit JSON representations for nonfinite rejected inputs, preserving rejection information and a valid audit schema.
- **Required regressions:** Older-handle rollback after later updates; parse every emitted line; quotes, backslashes, control characters, NaN/infinity rejection, and alias consistency.
- **Dependencies:** PB-10 target validation; document any serialization-schema additions. Cryptographic audit authentication is a separate limitation, not established by these tests.

## PB-13 — Raw-current energy ledger can overflow while model stability passes

- **Severity / confidence:** Medium; confirmed.
- **File / function:** `include/ds_bms_middleware_v2.hpp`, `ds_plugin::DSBMSMiddleware::enhance_cycle` energy-throughput accounting.
- **Reproduction:** Initialize `75 Ah`, `400 V`; call `enhance_cycle(360,DBL_MAX,25,0.8,1)`. DS normalizes current to `500 A` and returns a stable numerical model, while the raw `voltage * current` ledger becomes infinity.
- **Impact / invariant:** Finite sensor input can overflow the diagnostic ledger; model stability does not prove that middleware accounting remains finite. Raw fault detection and normalized model accounting have different purposes.
- **Minimal correction:** Validate derived accounting values and use a clearly defined accepted/raw-energy contract; stage ledger changes so invalid arithmetic cannot poison cumulative diagnostics.
- **Required regressions:** Extreme finite positive/negative current, long timestep overflow, normal energy units/signs, and preservation of diagnostics after rejected accounting.
- **Dependencies:** Raw fault preservation must remain intact; align accepted battery-energy accounting with PB-09.

## PB-14 — Advanced middleware exposes inconsistent nominal configuration

- **Severity / confidence:** Medium; numerical mismatch confirmed; the intended configuration-authority defect is suspected pending clarification of precedence.
- **File / function:** `include/ds_bms_middleware_v2.hpp`, `ds_plugin::DSBMSMiddleware::init_advanced` and `MiddlewareConfig`.
- **Reproduction:** Set outer nominal capacity `150 Ah`, nominal voltage `800 V`, leaving embedded `ds_config` at defaults `75 Ah`/`400 V`. After initialization, `enhance_cycle(360,10,25,0.5,1)` gives `energy_psi=4.86e7 J` rather than the outer-capacity value `9.72e7 J`, and charging recommendation voltage `400 V`.
- **Impact / invariant:** A caller can configure conflicting nominal values without rejection or explicit indication of which values own DS mathematics and charging recommendations. The mismatch is demonstrated; the intended authority needs a documented contract.
- **Minimal correction:** Define one authoritative nominal configuration or reject inconsistency. Forward/synchronize only after that precedence is established; avoid silently changing calibrated DS inputs.
- **Required regressions:** Matching defaults, matching nondefaults, conflicting capacity/voltage, and energy/charging outputs that follow the documented authority.
- **Dependencies:** Resolve configuration ownership first; preserve historical BEDROCK evidence and valid existing DS calibration behavior.

## PB-15 — Separate Python fallback accepts invalid timestep and SOC

- **Severity / confidence:** High; confirmed in the approximate Python fallback, not the C++ model.
- **File / function:** `ds_core/python_fallback/ds_enhancer_fallback.py`, `DSEnhancerFallback.enhance_cycle`.
- **Reproduction:** On a fresh instance, `enhance_cycle(360,10,25,0.8,-1)` leaves time `-1`, entropy `-0.05`, and reports `numerical_stability=True`. On another fresh instance, SOC NaN with `dt=1` is silently mapped to SOC `1` by ordered clamping and returns recommended temperature `30 C`, stability true.
- **Impact / invariant:** Malformed inputs mutate fallback state and produce a positive stability claim. This implementation is a separate approximate model; no numerical equivalence with C++ has been demonstrated.
- **Minimal correction:** Validate finite inputs and documented physical/timestep domains, calculate candidate fallback state, and commit only after all owned postconditions pass.
- **Required regressions:** Zero/negative/nonfinite timestep, NaN/infinite SOC and consumed sensors, extreme finite arithmetic, rejected-cycle atomicity, and existing valid fallback behavior.
- **Dependencies:** Keep fallback validation independent of repaired automotive-governor validation; document its model and software-validation limits.

## Architecture and assurance limits

The concrete trust-layer reference compartments are `BMSCompartment` and
`AnomalyCompartment`. Their source translation units largely include inline header
implementations. Thermal, charging, degradation compartments, a lock-free subsystem
bus, fleet parameter distribution, and staged OTA governance remain proposed phases
in `docs/AILEE_EV_INTEGRATION_ARCHITECTURE.md`; documented designs are not evidence
of implemented control paths. Existing thermal/charging helpers do not establish
those proposed compartment integrations.

LearningEngine is a reference API exercised by integration tests; no production
parameter-update/rollback call sites were found. This limits current execution
exposure but does not make its demonstrated authorization defects acceptable for
future integration.

Integrator-provided provenance, broader sensor freshness, audit authenticity,
chemistry calibration, and cross-thread ownership remain assurance limitations.
PB-07 and PB-08 identify specific reproducible evidence failures; they do not prove
that all external sensor or timing paths are defective. No untested theoretical
hazard is classified here as a confirmed defect.

The repository's AILEE Trust Contract 9.4 target is not independent verification of
binary/API compatibility with an upstream AILEE artifact. Such a claim requires an
obtainable authoritative source/version pin and independently compared interfaces,
semantics, and tests; this review establishes no such equivalence.

Software tests and synthetic simulation remain distinct from integration/SIL,
HIL, battery-pack testing, vehicle calibration, and vehicle certification. A
protective percentage or a passing test suite is not proof that every physical
fault condition permits that amount of power, torque, or charging. No physical
actuation, road test, charging, deployment, or automatic merge is authorized by
this backlog.
