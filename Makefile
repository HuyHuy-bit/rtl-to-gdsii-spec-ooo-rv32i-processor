PROFILE ?= s0-host
LOCKSTEP_SPIKE ?= ../riscv-isa-sim/build/spike
ACT4_CHECKOUT ?= out/deps/riscv-arch-test-4.0.0
ACT4_SAIL ?= out/deps/sail-riscv-0.10/bin/sail_riscv_sim
OSS_CAD_SUITE ?= $(HOME)/tools/oss-cad-suite-20260905/oss-cad-suite

.PHONY: doctor platform-generate platform-check event-generate event-check memory-generate memory-check lockstep-check act4-check act4-upstream-check prf-check prf-mutation-check a1-probe-check a1-synth-check check-fast check nightly reference-check
.PHONY: a1-timing-fetch a1-timing-check
.PHONY: a1-check
.PHONY: single-lane-check single-lane-synth-check
.PHONY: act4-tools act4-core-check
.PHONY: sail-log-check sail-differential-check
.PHONY: architectural-slice-check architectural-slice-evidence-check architectural-slice-checker-test
.PHONY: assert-portability
.PHONY: rename-ownership-check rename-recovery-ownership-check
.PHONY: backend-two-wide-check
.PHONY: issue-queue-check
.PHONY: issue-backend-check
.PHONY: integer-backend-check control-flow-backend-check fetch-two-wide-check fetch-execution-core-check frontend-faults-check
.PHONY: unit-runner-check
.PHONY: head-system-backend-check
.PHONY: serial-retirement-check
.PHONY: head-trap-core-check
.PHONY: csr-two-wide-check
.PHONY: rob-two-wide-check
.PHONY: rename-bundle-check
.PHONY: rename-state-check rename-checkpoints-check rename-recovery-check
.PHONY: formal-readiness-check formal-readiness-evidence-check formal-readiness-checker-test

doctor:
	@python3 tools/doctor.py --lock config/toolchain.lock --profile "$(PROFILE)"

assert-portability:
	@python3 tools/run_assert_portability.py --suite "$(OSS_CAD_SUITE)"

rename-ownership-check:
	@python3 tools/run_rename_ownership.py --suite "$(OSS_CAD_SUITE)"

rename-recovery-ownership-check:
	@python3 tools/run_rename_recovery_ownership.py --suite "$(OSS_CAD_SUITE)"

backend-two-wide-check:
	@python3 tools/run_unit.py backend_two_wide --suite "$(OSS_CAD_SUITE)"

issue-queue-check:
	@python3 tools/run_unit.py issue_queue --suite "$(OSS_CAD_SUITE)"

issue-backend-check:
	@python3 tools/run_unit.py issue_backend --suite "$(OSS_CAD_SUITE)"

unit-runner-check:
	@python3 -m unittest -v tests/test_unit_runner.py

head-system-backend-check:
	@python3 tools/run_unit.py head_system_backend --suite "$(OSS_CAD_SUITE)"

serial-retirement-check:
	@python3 tools/run_unit.py serial_retirement --suite "$(OSS_CAD_SUITE)"

head-trap-core-check:
	@python3 tools/run_unit.py head_trap_core --suite "$(OSS_CAD_SUITE)"

csr-two-wide-check:
	@python3 tools/run_unit.py csr_two_wide --suite "$(OSS_CAD_SUITE)"

frontend-faults-check:
	@python3 tools/run_unit.py frontend_faults --suite "$(OSS_CAD_SUITE)"

fetch-execution-core-check:
	@python3 tools/run_unit.py fetch_execution_core --suite "$(OSS_CAD_SUITE)"

fetch-two-wide-check:
	@python3 tools/run_unit.py fetch_two_wide --suite "$(OSS_CAD_SUITE)"

control-flow-backend-check:
	@python3 tools/run_unit.py control_flow_backend --suite "$(OSS_CAD_SUITE)"

integer-backend-check:
	@python3 tools/run_unit.py integer_backend --suite "$(OSS_CAD_SUITE)"

rob-two-wide-check:
	@python3 tools/run_unit.py rob_two_wide --suite "$(OSS_CAD_SUITE)"

rename-bundle-check:
	@python3 tools/run_unit.py rename_bundle --suite "$(OSS_CAD_SUITE)"

rename-state-check:
	@python3 tools/run_unit.py rename_state --suite "$(OSS_CAD_SUITE)"

rename-checkpoints-check:
	@python3 tools/run_unit.py rename_checkpoints --suite "$(OSS_CAD_SUITE)"

rename-recovery-check:
	@python3 tools/run_unit.py rename_recovery --suite "$(OSS_CAD_SUITE)"

formal-readiness-check:
	@python3 tools/run_formal_readiness.py --suite "$(OSS_CAD_SUITE)"

formal-readiness-evidence-check:
	@python3 tools/check_formal_readiness.py

formal-readiness-checker-test:
	@python3 -m unittest -v tests/test_formal_readiness.py

architectural-slice-check:
	@python3 tools/run_architectural_slice.py

architectural-slice-evidence-check:
	@python3 tools/check_architectural_slice.py

architectural-slice-checker-test:
	@python3 -m unittest -v tests/test_architectural_slice.py

platform-generate:
	@python3 tools/gen_platform.py --input config/platform.yaml --write

platform-check:
	@python3 tools/gen_platform.py --input config/platform.yaml --check
	@python3 -m unittest -v tests/test_platform.py

event-generate:
	@python3 tools/gen_commit_event.py --input config/commit_event.yaml --write

event-check:
	@python3 tools/gen_commit_event.py --input config/commit_event.yaml --check
	@python3 -m unittest -v tests/test_commit_event.py

memory-generate:
	@python3 tools/gen_memory_protocol.py --input config/memory_protocol.yaml --write

memory-check:
	@python3 tools/gen_memory_protocol.py --input config/memory_protocol.yaml --check
	@python3 -m unittest -v tests/test_memory_protocol.py

lockstep-check:
	@python3 -m unittest -v tests/test_lockstep.py
	@python3 tools/run_lockstep_smoke.py --spike "$(LOCKSTEP_SPIKE)"

act4-check:
	@python3 tools/check_act4.py
	@python3 -m unittest -v tests/test_act4_elf.py

act4-tools:
	@python3 tools/act4_tools.py --checkout "$(ACT4_CHECKOUT)"

act4-core-check:
	@python3 tools/run_act4_core.py --checkout "$(ACT4_CHECKOUT)" --sail "$(ACT4_SAIL)"

act4-upstream-check:
	@python3 tools/check_act4.py --checkout "$(ACT4_CHECKOUT)" --sail "$(ACT4_SAIL)"

prf-check:
	@python3 tools/run_prf_check.py

prf-mutation-check:
	@python3 tools/run_prf_check.py --mutations

a1-probe-check:
	@python3 tools/run_a1_probe.py

a1-synth-check:
	@python3 tools/run_a1_probe.py --synth --suite "$(OSS_CAD_SUITE)"

a1-timing-fetch:
	@python3 tools/fetch_a1_timing.py

a1-timing-check:
	@python3 tools/run_a1_timing.py --suite "$(OSS_CAD_SUITE)"

a1-check:
	@python3 -c 'from pathlib import Path; Path("out/a1/acceptance.json").unlink(missing_ok=True)'
	@$(MAKE) --no-print-directory prf-mutation-check
	@python3 tools/run_a1_probe.py --mutations
	@$(MAKE) --no-print-directory a1-synth-check a1-timing-check
	@python3 tools/check_a1.py --self-test

single-lane-check:
	@python3 tools/run_single_lane.py

sail-log-check:
	@python3 -m unittest -v tests/test_sail_log.py

sail-differential-check:
	@python3 tools/run_sail_differential.py --sail "$(ACT4_SAIL)"

single-lane-synth-check:
	@python3 tools/run_single_lane.py --synth --mutations --suite "$(OSS_CAD_SUITE)"

check-fast: unit-runner-check platform-check event-check memory-check lockstep-check act4-check prf-check a1-probe-check single-lane-check sail-log-check architectural-slice-checker-test
	@python3 tools/check_s0.py
	@git diff --check

check: doctor check-fast

nightly: check reference-check

reference-check:
	@$(MAKE) --no-print-directory -C verif/reference/rv32i check
