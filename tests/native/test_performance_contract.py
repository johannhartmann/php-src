#!/usr/bin/env python3

from __future__ import annotations

import importlib.util
import platform
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "native" / "benchmark-native-performance.py"
SPEC = importlib.util.spec_from_file_location("native_performance_benchmark", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
BENCHMARK = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = BENCHMARK
SPEC.loader.exec_module(BENCHMARK)


class PerformanceContractTests(unittest.TestCase):
    def test_default_sample_count_supports_a_real_p95(self) -> None:
        self.assertGreaterEqual(BENCHMARK.DEFAULT_SAMPLES, 20)
        self.assertEqual(
            19.0,
            BENCHMARK.percentile(range(1, BENCHMARK.DEFAULT_SAMPLES + 1), 0.95),
        )

    def test_independent_scaling_case_keeps_one_static_dependency(self) -> None:
        source = BENCHMARK.independent_source(1000)

        self.assertIn("return unused_500($v);", source)

    def test_product_cli_summary_defines_cold_compile_latency_as_wall_time(
        self,
    ) -> None:
        descriptor = BENCHMARK.Benchmark(
            "scalar_return", "direct", "<?php", "root", (), 1, repeat=1
        )
        record = BENCHMARK.summarize(
            descriptor,
            [
                {
                    "return_value": 1,
                    "execute_ns": 100,
                    "wall_ns": 120,
                    "peak_rss_bytes": 1200,
                }
            ],
            [
                {
                    "return_value": 1,
                    "execute_ns": 110,
                    "wall_ns": 100,
                    "peak_rss_bytes": 1000,
                }
            ],
            [{"return_value": 1, "execute_ns": 300}],
            [{"wall_ns": 120}],
            None,
            [{"wall_ns": 100}],
            mode="product-cli",
            opcache=True,
            warm_probe=[{"wall_ns": 60, "opcache_hits": 1}],
        )

        self.assertEqual(3.0, record["candidate_vs_reference"])
        self.assertNotIn("cold_compile_p95_ratio", record)
        self.assertNotIn("native_compile_p95_ns", record)
        self.assertEqual(1.2, record["product_cold_compile_latency_p95_ratio"])
        self.assertIn(
            "first request in a fresh CLI process",
            record["product_cold_compile_latency_definition"],
        )
        self.assertIn(
            "empty OPcache file cache",
            record["product_cold_compile_latency_definition"],
        )
        self.assertEqual(1.2, record["peak_rss_ratio"])
        self.assertEqual(2.0, record["warm_vs_cold"])

    def test_product_cli_discards_one_role_local_loader_warmup(self) -> None:
        descriptor = BENCHMARK.Benchmark(
            "scalar_return", "direct", "<?php", "root", (200_000,), 200_000
        )
        with patch.object(
            BENCHMARK,
            "run_product_cli",
            return_value={"status": "returned", "return_value": 1},
        ) as run_cli:
            measured, cold, prime, warm_probe = BENCHMARK.measure_product(
                Path("/php"),
                Path("/benchmark.php"),
                descriptor,
                "darwin-arm64-dev",
                3,
                False,
                Path("/opcache"),
                None,
            )

        self.assertEqual(7, run_cli.call_count)
        self.assertEqual(3, len(measured))
        self.assertEqual(3, len(cold))
        self.assertIsNone(prime)
        self.assertIsNone(warm_probe)
        cold_descriptors = [
            call.args[2] for call in run_cli.call_args_list[:4]
        ]
        self.assertTrue(
            all(item.arguments == (1,) for item in cold_descriptors)
        )
        self.assertTrue(all(item.repeat == 1 for item in cold_descriptors))
        measured_descriptors = [
            call.args[2] for call in run_cli.call_args_list[4:]
        ]
        self.assertTrue(all(item is descriptor for item in measured_descriptors))

    def test_scaling_cold_probe_preserves_dependency_depth(self) -> None:
        descriptor = BENCHMARK.Benchmark(
            "scc_100", "scaling", "<?php", "root", (100,), 101, repeat=1
        )

        cold = BENCHMARK.product_cold_benchmark(descriptor)

        self.assertEqual((100,), cold.arguments)
        self.assertEqual(1, cold.repeat)

    def test_product_roles_rotate_sample_order(self) -> None:
        descriptor = BENCHMARK.Benchmark(
            "scalar_return", "direct", "<?php", "root", (200_000,), 200_000
        )
        roles = (
            BENCHMARK.ProductRole(
                "candidate", Path("/candidate"), Path("/candidate-cache"), None
            ),
            BENCHMARK.ProductRole(
                "baseline", Path("/baseline"), Path("/baseline-cache"), None
            ),
        )
        with patch.object(
            BENCHMARK,
            "run_product_cli",
            return_value={"status": "returned", "return_value": 1},
        ) as run_cli:
            measurements, errors = BENCHMARK.measure_product_roles(
                roles,
                Path("/benchmark.php"),
                descriptor,
                "darwin-arm64-dev",
                2,
                False,
            )

        self.assertEqual({}, errors)
        self.assertEqual(2, len(measurements["candidate"][0]))
        self.assertEqual(2, len(measurements["baseline"][0]))
        self.assertEqual(
            [
                Path("/candidate"),
                Path("/baseline"),
                Path("/candidate"),
                Path("/baseline"),
                Path("/baseline"),
                Path("/candidate"),
                Path("/baseline"),
                Path("/candidate"),
                Path("/candidate"),
                Path("/baseline"),
            ],
            [call.args[0] for call in run_cli.call_args_list],
        )

    def test_product_fpm_summary_uses_fresh_script_first_request(self) -> None:
        descriptor = BENCHMARK.Benchmark(
            "scalar_return", "direct", "<?php", "root", (), 1, repeat=1
        )
        record = BENCHMARK.summarize(
            descriptor,
            [{"return_value": 1, "execute_ns": 100, "wall_ns": 80}],
            [{"return_value": 1, "execute_ns": 110, "wall_ns": 70}],
            None,
            [{"wall_ns": 120}],
            None,
            [{"wall_ns": 100}],
            mode="product-fpm",
            opcache=True,
        )

        self.assertEqual(1.2, record["product_cold_compile_latency_p95_ratio"])
        self.assertNotIn("cold_compile_p95_ratio", record)
        self.assertNotIn("native_compile_p95_ns", record)
        self.assertIn(
            "first request to a fresh script path",
            record["product_cold_compile_latency_definition"],
        )
        self.assertIn(
            "persistent FPM worker",
            record["product_cold_compile_latency_definition"],
        )

    def test_incompatible_baseline_is_recorded_without_hiding_candidate_result(
        self,
    ) -> None:
        descriptor = BENCHMARK.Benchmark(
            "self_recursion", "direct", "<?php", "root", (), 1, repeat=1
        )
        record = BENCHMARK.summarize(
            descriptor,
            [
                {
                    "return_value": 10,
                    "execute_ns": 100,
                    "wall_ns": 120,
                    "peak_rss_bytes": 1200,
                }
            ],
            [
                {
                    "return_value": 5,
                    "execute_ns": 20,
                    "wall_ns": 80,
                    "peak_rss_bytes": 1000,
                }
            ],
            [{"return_value": 10, "execute_ns": 300}],
            [{"wall_ns": 120}],
            None,
            [{"wall_ns": 80}],
            mode="product-cli",
            opcache=False,
        )

        self.assertIn(
            "candidate returned 10, baseline returned 5",
            record["baseline_error"],
        )
        self.assertEqual(3.0, record["candidate_vs_reference"])
        self.assertNotIn("speedup", record)
        self.assertNotIn("peak_rss_ratio", record)
        self.assertNotIn("product_cold_compile_latency_p95_ratio", record)

        summary = {
            "direct_scalar_speedup": BENCHMARK.V1_MIN_BASELINE_SPEEDUP,
            "direct_scalar_vs_reference": 2.0,
        }
        self.assertEqual(
            [], BENCHMARK.v1_product_contract_failures([record], summary)
        )

    def test_summary_uses_native_compile_metric_when_exposed(self) -> None:
        descriptor = BENCHMARK.Benchmark(
            "scalar_return", "direct", "<?php", "root", (), 1, repeat=1
        )
        record = BENCHMARK.summarize(
            descriptor,
            [
                {
                    "return_value": 1,
                    "execute_ns": 100,
                    "performance": {"compile_ns": 120},
                }
            ],
            [
                {
                    "return_value": 1,
                    "execute_ns": 110,
                    "performance": {"compile_ns": 100},
                }
            ],
            [{"return_value": 1, "execute_ns": 300}],
        )

        self.assertEqual(1.2, record["cold_compile_p95_ratio"])
        self.assertNotIn("product_cold_compile_latency_p95_ratio", record)
        self.assertIn(
            "native compiler compile_ns",
            record["cold_measurement_definition"],
        )

    def test_v1_product_thresholds_are_hard_boundaries(self) -> None:
        records = [
            {
                "suite": "direct",
                "case": "scalar_return",
                "product_cold_compile_latency_p95_ratio": 1.2,
                "peak_rss_ratio": 1.2,
            },
            {
                "suite": "hot",
                "case": "packed_array_read",
                "product_cold_compile_latency_p95_ratio": 1.2,
                "peak_rss_ratio": 1.2,
            },
        ]
        passing = {
            "direct_scalar_speedup": BENCHMARK.V1_MIN_BASELINE_SPEEDUP,
            "direct_scalar_vs_reference": 2.0,
            "hot_geomean_speedup": BENCHMARK.V1_MIN_BASELINE_SPEEDUP,
            "hot_geomean_vs_reference": 1.25,
        }

        self.assertEqual([], BENCHMARK.v1_product_contract_failures(records, passing))

        records[0]["product_cold_compile_latency_p95_ratio"] = 1.2001
        records[1]["peak_rss_ratio"] = 1.2001
        failing = {
            "direct_scalar_speedup": BENCHMARK.V1_MIN_BASELINE_SPEEDUP - 0.01,
            "direct_scalar_vs_reference": 1.99,
            "hot_geomean_speedup": BENCHMARK.V1_MIN_BASELINE_SPEEDUP - 0.01,
            "hot_geomean_vs_reference": 1.24,
        }
        failures = BENCHMARK.v1_product_contract_failures(records, failing)

        self.assertEqual(6, len(failures))
        self.assertTrue(any("2.0x" in failure for failure in failures))
        self.assertTrue(any("1.25x" in failure for failure in failures))
        self.assertTrue(any("compile latency p95" in failure for failure in failures))
        self.assertTrue(any("peak RSS" in failure for failure in failures))

    def test_diagnostic_compile_threshold_is_mandatory(self) -> None:
        passing = [
            {
                "suite": "direct",
                "case": "scalar_return",
                "cold_compile_p95_ratio": 1.2,
            },
            {
                "suite": "hot",
                "case": "packed_array_read",
                "cold_compile_p95_ratio": 1.2,
            },
        ]
        self.assertEqual([], BENCHMARK.v1_diagnostic_contract_failures(passing))

        passing[0]["cold_compile_p95_ratio"] = 1.2001
        del passing[1]["cold_compile_p95_ratio"]
        failures = BENCHMARK.v1_diagnostic_contract_failures(passing)

        self.assertEqual(2, len(failures))
        self.assertTrue(any("comparison missing" in failure for failure in failures))
        self.assertTrue(any("more than 20%" in failure for failure in failures))

    def test_product_mode_does_not_require_diagnostic_metrics(self) -> None:
        records = [
            {
                "suite": "direct",
                "case": "scalar_return",
                "product_cold_compile_latency_p95_ratio": 1.0,
                "peak_rss_ratio": 1.0,
            }
        ]
        summary = {
            "direct_scalar_speedup": 1.0,
            "direct_scalar_vs_reference": 2.0,
        }

        self.assertEqual(
            [], BENCHMARK.v1_mode_contract_failures("product-cli", records, summary)
        )

    def test_product_fpm_gate_reports_fail_closed_architectural_evidence(
        self,
    ) -> None:
        script = (ROOT / "scripts" / "native" / "test-native-product-fpm.sh").read_text(
            encoding="utf-8"
        )

        self.assertNotIn("native_counter_evidence=unavailable", script)
        self.assertIn("verify_fail_closed_product_architecture", script)
        self.assertIn(
            "profile_root=$(CDPATH='' cd -- \"$candidate_sapi_root/../..\"",
            script,
        )
        self.assertIn(
            "product_manifest=$profile_root/build-manifest.json",
            script,
        )
        self.assertIn("native_build_manifest=verified", script)
        self.assertIn("include_generations=3", script)
        self.assertIn("suspended_fiber_reset=1", script)
        self.assertIn("include_cached_before_reset=yes", script)
        self.assertLess(
            script.index("opcache_reset()"), script.index("$fiber->resume(10)")
        )
        self.assertIn(
            "native_execution_evidence=fail_closed_product_executor_and_"
            "successful_fpm_userland",
            script,
        )

    def test_opcache_state_is_validated_per_product_sample(self) -> None:
        descriptor = BENCHMARK.Benchmark(
            "scalar_return", "direct", "<?php", "root", (), 1, repeat=1
        )
        enabled = {
            "opcache_enabled": True,
            "opcache_hits": 1,
            "opcache_misses": 0,
            "opcache_cached_scripts": 1,
        }
        disabled = {
            "opcache_enabled": False,
            "opcache_hits": 0,
            "opcache_misses": 0,
            "opcache_cached_scripts": 0,
        }

        BENCHMARK.validate_product_opcache_samples(
            descriptor, "candidate warm", [enabled], True, require_hit=True
        )
        BENCHMARK.validate_product_opcache_samples(
            descriptor, "candidate warm", [disabled], False, require_hit=False
        )

        missing_hit = dict(enabled, opcache_hits=0)
        with self.assertRaisesRegex(RuntimeError, "recorded no OPcache hit"):
            BENCHMARK.validate_product_opcache_samples(
                descriptor,
                "candidate warm",
                [missing_hit],
                True,
                require_hit=True,
            )
        with self.assertRaisesRegex(RuntimeError, "expected True"):
            BENCHMARK.validate_product_opcache_samples(
                descriptor,
                "candidate warm",
                [disabled],
                True,
                require_hit=True,
            )
        invalid_disabled = dict(disabled, opcache_hits=1)
        with self.assertRaisesRegex(RuntimeError, "statistics while disabled"):
            BENCHMARK.validate_product_opcache_samples(
                descriptor,
                "candidate warm",
                [invalid_disabled],
                False,
                require_hit=False,
            )

    def test_structural_metrics_fail_closed(self) -> None:
        passing = [
            {
                "suite": "direct",
                "case": "scalar_return",
                "inner_call_runtime_helper_calls": 0,
                "inner_call_heap_allocations": 0,
                "inner_call_catcher_boundaries": 0,
                "direct_typed_body_sites": 1,
                "direct_call_frame_bytes": 0,
            }
        ]
        self.assertEqual([], BENCHMARK.v1_structural_contract_failures(passing))

        failing = [
            {
                "suite": "direct",
                "case": "generic_frame",
                "inner_call_runtime_helper_calls": 2,
                "inner_call_heap_allocations": 1,
                "inner_call_catcher_boundaries": 1,
                "direct_typed_body_sites": 1,
                "direct_call_frame_bytes": 96,
            },
            {"suite": "direct", "case": "missing_metrics"},
        ]
        failures = BENCHMARK.v1_structural_contract_failures(failing)

        self.assertEqual(9, len(failures))
        self.assertTrue(any("runtime_helper_calls=2" in item for item in failures))
        self.assertTrue(any("heap_allocations=1" in item for item in failures))
        self.assertTrue(any("catcher_boundaries=1" in item for item in failures))
        self.assertTrue(any("frame bytes=96" in item for item in failures))
        self.assertTrue(any("direct_typed_body_sites=None" in item for item in failures))
        self.assertTrue(any("direct_call_frame_bytes=None" in item for item in failures))
        self.assertTrue(any("missing_metrics" in item for item in failures))

    def test_peak_rss_contract_uses_p95_instead_of_median(self) -> None:
        descriptor = BENCHMARK.Benchmark(
            "scalar_return", "direct", "<?php", "root", (), 1, repeat=1
        )
        candidate = [
            {"return_value": 1, "execute_ns": 100, "peak_rss_bytes": value}
            for value in (100, 101, 102, 103, 200)
        ]
        baseline = [
            {"return_value": 1, "execute_ns": 100, "peak_rss_bytes": value}
            for value in (100, 100, 100, 100, 100)
        ]

        record = BENCHMARK.summarize(
            descriptor,
            candidate,
            baseline,
            None,
            mode="product-cli",
            opcache=False,
        )

        self.assertEqual(200.0, record["candidate_peak_rss_bytes"])
        self.assertEqual(2.0, record["peak_rss_ratio"])
        self.assertIn("p95", record["peak_rss_definition"])

    def test_python_entrypoints_use_stable_usage_and_prerequisite_exits(
        self,
    ) -> None:
        target = BENCHMARK.TARGET_BY_HOST[(platform.system(), platform.machine())]
        benchmark_script = str(SCRIPT)
        execution_script = str(ROOT / "scripts" / "native" / "test-native-execution.py")
        missing = str(ROOT / "tests" / "native" / "does-not-exist-php")

        invalid_samples = subprocess.run(
            [
                sys.executable,
                benchmark_script,
                "--candidate",
                sys.executable,
                "--target",
                target,
                "--samples",
                "0",
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(2, invalid_samples.returncode)
        self.assertIn("--samples must be positive", invalid_samples.stderr)
        self.assertNotIn("Traceback", invalid_samples.stderr)

        missing_candidate = subprocess.run(
            [
                sys.executable,
                benchmark_script,
                "--candidate",
                missing,
                "--target",
                target,
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(3, missing_candidate.returncode)
        self.assertIn("candidate PHP binary is unavailable", missing_candidate.stderr)
        self.assertNotIn("Traceback", missing_candidate.stderr)

        same_binary = subprocess.run(
            [
                sys.executable,
                execution_script,
                "--target",
                target,
                "--candidate",
                sys.executable,
                "--reference",
                sys.executable,
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(2, same_binary.returncode)
        self.assertIn("must differ", same_binary.stderr)
        self.assertNotIn("Traceback", same_binary.stderr)

        missing_execution_input = subprocess.run(
            [
                sys.executable,
                execution_script,
                "--target",
                target,
                "--candidate",
                missing,
                "--reference",
                sys.executable,
            ],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=False,
        )
        self.assertEqual(3, missing_execution_input.returncode)
        self.assertIn("candidate PHP binary is unavailable", missing_execution_input.stderr)
        self.assertNotIn("Traceback", missing_execution_input.stderr)


if __name__ == "__main__":
    unittest.main()
