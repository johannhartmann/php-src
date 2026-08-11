#!/usr/bin/env python3
"""Measure the native baseline without creating a benchmark ledger."""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import json
import math
import os
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, replace
from pathlib import Path
from typing import Any, Iterable


TARGET_BY_HOST = {
    ("Darwin", "arm64"): "darwin-arm64-dev",
    ("Linux", "x86_64"): "linux-amd64-prod",
}

V1_MIN_BASELINE_SPEEDUP = 1 / 1.10
V1_MIN_DIRECT_REFERENCE_SPEEDUP = 2.0
V1_MIN_HOT_REFERENCE_SPEEDUP = 1.25
V1_MAX_RESOURCE_RATIO = 1.20
V1_LAZY_CODEUNITS = 2
DEFAULT_SAMPLES = 20


class UsageError(RuntimeError):
    """An invalid caller-supplied option or option combination."""


class PrerequisiteError(RuntimeError):
    """A required host capability, executable, or input is unavailable."""


CANDIDATE_RUNNER = r"""
$source = file_get_contents(getenv("NATIVE_BENCH_SOURCE_FILE"));
if ($source === false) {
    throw new RuntimeException("cannot read benchmark source");
}
$arguments = json_decode(
    getenv("NATIVE_BENCH_ARGUMENTS"), true, 512, JSON_THROW_ON_ERROR
);
$options = [
    "wave" => 11,
    "function" => getenv("NATIVE_BENCH_FUNCTION"),
    "target" => getenv("NATIVE_BENCH_TARGET"),
    "repeat" => (int) getenv("NATIVE_BENCH_REPEAT"),
];
$started = hrtime(true);
$result = native_mir_test_compile_execute(
    $source, getenv("NATIVE_BENCH_FILENAME"), $arguments, $options
);
$bridgeNs = hrtime(true) - $started;
$execution = $result["execution"] ?? [];
$usage = getrusage();
$peakRss = (int) ($usage["ru_maxrss"] ?? 0);
if (str_starts_with(getenv("NATIVE_BENCH_TARGET"), "linux-")) {
    $peakRss *= 1024;
}
echo json_encode(
    [
        "status" => $result["status"] ?? "missing",
        "phase" => $result["phase"] ?? "missing",
        "return_value" => $execution["return_value"] ?? null,
        "bridge_ns" => $bridgeNs,
        "performance" => $execution["performance"] ?? null,
        "native_codeunits" => $execution["native_codeunits"] ?? null,
        "native_components" => $execution["native_components"] ?? null,
        "image_size" => $execution["image_size"] ?? null,
        "vm_handler_calls" => $execution["vm_handler_calls"] ?? null,
        "execute_ex_calls" => $execution["execute_ex_calls"] ?? null,
        "opline_handler_calls" => $execution["opline_handler_calls"] ?? null,
        "peak_rss_bytes" => $peakRss,
    ],
    JSON_PRESERVE_ZERO_FRACTION | JSON_THROW_ON_ERROR
);
"""

REFERENCE_RUNNER = r"""
$source = file_get_contents(getenv("NATIVE_BENCH_SOURCE_FILE"));
$arguments = json_decode(
    getenv("NATIVE_BENCH_ARGUMENTS"), true, 512, JSON_THROW_ON_ERROR
);
if ($source === false || !str_starts_with($source, "<?php")) {
    throw new RuntimeException("invalid benchmark source");
}
eval(substr($source, 5));
$function = getenv("NATIVE_BENCH_FUNCTION");
$repeat = (int) getenv("NATIVE_BENCH_REPEAT");
$started = hrtime(true);
for ($index = 0; $index < $repeat; $index++) {
    $result = $function(...$arguments);
}
$executeNs = hrtime(true) - $started;
$usage = getrusage();
$peakRss = (int) ($usage["ru_maxrss"] ?? 0);
if (str_starts_with(getenv("NATIVE_BENCH_TARGET"), "linux-")) {
    $peakRss *= 1024;
}
echo json_encode(
    [
        "status" => "returned",
        "return_value" => $result,
        "execute_ns" => $executeNs,
        "peak_rss_bytes" => $peakRss,
    ],
    JSON_PRESERVE_ZERO_FRACTION | JSON_THROW_ON_ERROR
);
"""

PRODUCT_RUNNER = r"""
$arguments = json_decode(
    getenv("NATIVE_BENCH_ARGUMENTS"), true, 512, JSON_THROW_ON_ERROR
);
$repeat = (int) getenv("NATIVE_BENCH_REPEAT");
$started = hrtime(true);
for ($index = 0; $index < $repeat; $index++) {
    $result = __NATIVE_BENCH_FUNCTION__(...$arguments);
}
$executeNs = hrtime(true) - $started;
$usage = getrusage();
$peakRss = (int) ($usage["ru_maxrss"] ?? 0);
if (str_starts_with(getenv("NATIVE_BENCH_TARGET"), "linux-")) {
    $peakRss *= 1024;
}
$opcache = function_exists("opcache_get_status")
    ? opcache_get_status(false) : false;
$statistics = is_array($opcache)
    ? ($opcache["opcache_statistics"] ?? []) : [];
echo json_encode(
    [
        "status" => "returned",
        "return_value" => $result,
        "execute_ns" => $executeNs,
        "peak_rss_bytes" => $peakRss,
        "opcache_enabled" => is_array($opcache),
        "opcache_hits" => (int) ($statistics["hits"] ?? 0),
        "opcache_misses" => (int) ($statistics["misses"] ?? 0),
        "opcache_cached_scripts" => (int) (
            $statistics["num_cached_scripts"] ?? 0
        ),
    ],
    JSON_PRESERVE_ZERO_FRACTION | JSON_THROW_ON_ERROR
);
"""


@dataclass(frozen=True)
class Benchmark:
    name: str
    suite: str
    source: str
    function: str
    arguments: tuple[Any, ...]
    operations: int
    repeat: int = 10
    ini: tuple[str, ...] = ()


def direct_benchmarks(iterations: int) -> tuple[Benchmark, ...]:
    cases = {
        "empty_user_function": """
function dc_empty_leaf(): void {}
function dc_empty_root(int $n): int {
    for ($i = 0; $i < $n; $i++) { dc_empty_leaf(); }
    return $n;
}
""",
        "one_scalar_argument": """
function dc_one_leaf(int $value): int { return $value + 1; }
function dc_one_root(int $n): int {
    $value = 0;
    for ($i = 0; $i < $n; $i++) { $value = dc_one_leaf($value); }
    return $value;
}
""",
        "eight_scalar_arguments": """
function dc_eight_leaf(
    int $a, int $b, int $c, int $d, int $e, int $f, int $g, int $h
): int { return $a + $h; }
function dc_eight_root(int $n): int {
    $value = 0;
    for ($i = 0; $i < $n; $i++) {
        $value += dc_eight_leaf(1, 2, 3, 4, 5, 6, 7, 8);
    }
    return $value;
}
""",
        "sixteen_mixed_arguments": """
function dc_mixed_leaf(
    mixed $a, mixed $b, mixed $c, mixed $d,
    mixed $e, mixed $f, mixed $g, mixed $h,
    mixed $i, mixed $j, mixed $k, mixed $l,
    mixed $m, mixed $n, mixed $o, mixed $p
): int { return 1; }
function dc_mixed_root(int $count): int {
    $value = 0;
    for ($i = 0; $i < $count; $i++) {
        $value += dc_mixed_leaf(
            1, 2.0, true, null, 'x', [1], 7, 8,
            9, 10.0, false, null, 'y', [2], 15, 16
        );
    }
    return $value;
}
""",
        "boxed_zval_argument": """
function dc_boxed_leaf(mixed $value): mixed { return $value; }
function dc_boxed_root(int $n): int {
    $value = [1, 2, 3];
    for ($i = 0; $i < $n; $i++) { $value = dc_boxed_leaf($value); }
    return $value[0];
}
""",
        "refcounted_string_argument": """
function dc_string_leaf(string $value): int { return strlen($value); }
function dc_string_root(int $n): int {
    $value = 0;
    $text = 'native-string';
    for ($i = 0; $i < $n; $i++) { $value += dc_string_leaf($text); }
    return $value;
}
""",
        "by_reference_argument": """
function dc_ref_leaf(int &$value): void { $value++; }
function dc_ref_root(int $n): int {
    $value = 0;
    for ($i = 0; $i < $n; $i++) { dc_ref_leaf($value); }
    return $value;
}
""",
        "scalar_return": """
function dc_scalar_leaf(int $value): int { return $value + 1; }
function dc_scalar_root(int $n): int {
    $value = 0;
    for ($i = 0; $i < $n; $i++) { $value = dc_scalar_leaf($value); }
    return $value;
}
""",
        "zval_return": """
function dc_zval_leaf(int $value): array { return [$value]; }
function dc_zval_root(int $n): int {
    $value = [0];
    for ($i = 0; $i < $n; $i++) { $value = dc_zval_leaf($value[0] + 1); }
    return $value[0];
}
""",
        "self_recursion": """
function dc_self_leaf(int $depth): int {
    return $depth === 0 ? 1 : dc_self_leaf($depth - 1) + 1;
}
function dc_self_root(int $n): int {
    $value = 0;
    for ($i = 0; $i < $n; $i++) { $value += dc_self_leaf(4); }
    return $value;
}
""",
        "mutual_recursion": """
function dc_mutual_a(int $depth): int {
    return $depth === 0 ? 1 : dc_mutual_b($depth - 1) + 1;
}
function dc_mutual_b(int $depth): int {
    return $depth === 0 ? 1 : dc_mutual_a($depth - 1) + 1;
}
function dc_mutual_root(int $n): int {
    $value = 0;
    for ($i = 0; $i < $n; $i++) { $value += dc_mutual_a(4); }
    return $value;
}
""",
        "call_in_loop": """
function dc_loop_leaf(int $value): int { return $value + 1; }
function dc_loop_root(int $n): int {
    $value = 0;
    for ($i = 0; $i < $n; $i++) { $value = dc_loop_leaf($value); }
    return $value;
}
""",
        "observer_disabled": """
function dc_observer_leaf(int $value): int { return $value + 1; }
function dc_observer_root(int $n): int {
    $value = 0;
    for ($i = 0; $i < $n; $i++) { $value = dc_observer_leaf($value); }
    return $value;
}
""",
        "observer_enabled": """
function dc_observed_leaf(int $value): int { return $value + 1; }
function dc_observed_root(int $n): int {
    $value = 0;
    for ($i = 0; $i < $n; $i++) { $value = dc_observed_leaf($value); }
    return $value;
}
""",
    }
    result = []
    for name, body in cases.items():
        function = {
            "empty_user_function": "dc_empty_root",
            "one_scalar_argument": "dc_one_root",
            "eight_scalar_arguments": "dc_eight_root",
            "sixteen_mixed_arguments": "dc_mixed_root",
            "boxed_zval_argument": "dc_boxed_root",
            "refcounted_string_argument": "dc_string_root",
            "by_reference_argument": "dc_ref_root",
            "scalar_return": "dc_scalar_root",
            "zval_return": "dc_zval_root",
            "self_recursion": "dc_self_root",
            "mutual_recursion": "dc_mutual_root",
            "call_in_loop": "dc_loop_root",
            "observer_disabled": "dc_observer_root",
            "observer_enabled": "dc_observed_root",
        }[name]
        ini = ()
        if name == "observer_enabled":
            ini = (
                "zend_test.observer.enabled=1",
                "zend_test.observer.show_output=0",
                "zend_test.observer.observe_all=1",
            )
        result.append(
            Benchmark(
                name,
                "direct",
                "<?php\n" + body,
                function,
                (iterations,),
                iterations,
                ini=ini,
            )
        )
    return tuple(result)


def hot_benchmarks(iterations: int, include_file: Path) -> tuple[Benchmark, ...]:
    path = str(include_file).replace("\\", "\\\\").replace("'", "\\'")
    definitions = (
        (
            "cv_assignment_loop",
            "function hot_cv(int $n): int {"
            "$a=1;$b=0;for($i=0;$i<$n;$i++){$b=$a;}return $b;}",
            "hot_cv",
        ),
        (
            "isset_empty_loop",
            "function hot_isset(int $n): int {"
            "$v=1;$r=0;for($i=0;$i<$n;$i++){"
            "if(isset($v)&&!empty($v)){$r++;}}return $r;}",
            "hot_isset",
        ),
        (
            "string_truthiness_length",
            "function hot_string(int $n): int {"
            "$s='native';$r=0;for($i=0;$i<$n;$i++){"
            "if($s){$r+=strlen($s);}}return $r;}",
            "hot_string",
        ),
        (
            "packed_array_read",
            "function hot_packed_read(int $n): int {"
            "$a=[1,2,3,4,5,6,7,8];$r=0;for($i=0;$i<$n;$i++){"
            "$k=$i&7;$r+=$a[$k];}return $r;}",
            "hot_packed_read",
        ),
        (
            "packed_array_append",
            "function hot_packed_append(int $n): int {"
            "$a=[];for($i=0;$i<$n;$i++){$a[]=$i;}"
            "return $n===0?0:$a[$n-1];}",
            "hot_packed_append",
        ),
        (
            "mixed_array_cached_read",
            "function hot_mixed_read(int $n): int {"
            "$a=['native'=>7];$k='native';$r=0;"
            "for($i=0;$i<$n;$i++){$r+=$a[$k];}return $r;}",
            "hot_mixed_read",
        ),
        (
            "standard_property_cached_read",
            "class HotRead{public int $value=7;}"
            "function hot_property_read(int $n): int {"
            "$o=new HotRead();$r=0;for($i=0;$i<$n;$i++){$r+=$o->value;}"
            "return $r;}",
            "hot_property_read",
        ),
        (
            "standard_property_cached_write",
            "class HotWrite{public int $value=0;}"
            "function hot_property_write(int $n): int {"
            "$o=new HotWrite();for($i=0;$i<$n;$i++){$o->value=$i;}"
            "return $o->value;}",
            "hot_property_write",
        ),
        (
            "simple_method_loop",
            "final class HotMethod{"
            "public function step(int $v):int{return $v+1;}}"
            "function hot_method(int $n): int {"
            "$o=new HotMethod();$r=0;for($i=0;$i<$n;$i++){$r=$o->step($r);}"
            "return $r;}",
            "hot_method",
        ),
        (
            "foreach",
            "function hot_foreach(int $n): int {"
            "$a=[1,2,3,4];$r=0;for($i=0;$i<$n;$i++){"
            "foreach($a as $v){$r+=$v;}}return $r;}",
            "hot_foreach",
        ),
        (
            "dynamic_variable_lookup",
            "function hot_dynamic(int $n): int {"
            "$name='value';$value=7;$r=0;for($i=0;$i<$n;$i++){"
            "$r+=$$name;}return $r;}",
            "hot_dynamic",
        ),
        (
            "include_once_hit",
            f"function hot_include(int $n): int {{"
            f"for($i=0;$i<$n;$i++){{include_once '{path}';}}return $n;}}",
            "hot_include",
        ),
    )
    return tuple(
        Benchmark(
            name,
            "hot",
            "<?php\n" + source,
            function,
            (iterations,),
            iterations,
        )
        for name, source, function in definitions
    )


def independent_source(count: int) -> str:
    functions = "\n".join(
        f"function unused_{index}(int $v): int {{ return $v + {index}; }}"
        for index in range(count)
    )
    return (
        "<?php\n"
        + functions
        + "\nfunction scaling_root(int $v): int {"
        + f" return unused_{count // 2}($v); }}\n"
    )


def scc_source(count: int) -> str:
    functions = []
    for index in range(count):
        next_index = (index + 1) % count
        functions.append(
            f"function scc_{index}(int $n): int {{"
            f"return $n === 0 ? {index} : scc_{next_index}($n - 1); }}"
        )
    return (
        "<?php\n"
        + "\n".join(functions)
        + "\nfunction scc_root(int $n): int { return scc_0($n); }\n"
    )


def method_source(count: int) -> str:
    classes = "\n".join(
        f"class UnusedClass{index} {{"
        f"public function value(int $v): int {{ return $v + {index}; }} }}"
        for index in range(count)
    )
    return (
        "<?php\n" + classes + "\nclass UsedClass {"
        "public function value(int $v): int { return $v + 1; }}"
        "function method_scaling_root(int $v): int {"
        "$o = new UsedClass(); return $o->value($v); }\n"
    )


def dynamic_include_source(count: int) -> str:
    functions = "\n".join(
        f"function dynamic_unused_{count}_{index}(int $v): int {{"
        f" return $v + {index}; }}"
        for index in range(count)
    )
    return (
        "<?php\n" + functions + f"\nfunction dynamic_used_{count}(int $v): int {{"
        " return $v + 1; }\n"
    )


def dynamic_include_root(count: int) -> str:
    return (
        "<?php\n"
        f"function dynamic_include_root_{count}(string $file): int {{\n"
        "    include $file;\n"
        f"    return dynamic_used_{count}(1);\n"
        "}\n"
    )


def scaling_benchmarks(quick: bool, temporary_directory: Path) -> tuple[Benchmark, ...]:
    independent_counts = (100, 1000) if quick else (100, 1000, 5000)
    scc_counts = (2, 10) if quick else (2, 10, 100)
    method_counts = (100,) if quick else (100, 1000)
    result = []
    for count in independent_counts:
        result.append(
            Benchmark(
                f"independent_{count}",
                "scaling",
                independent_source(count),
                "scaling_root",
                (1,),
                1,
                repeat=1,
            )
        )
    for count in scc_counts:
        result.append(
            Benchmark(
                f"scc_{count}",
                "scaling",
                scc_source(count),
                "scc_root",
                (count,),
                count + 1,
                repeat=1,
            )
        )
    for count in method_counts:
        result.append(
            Benchmark(
                f"methods_{count}",
                "scaling",
                method_source(count),
                "method_scaling_root",
                (1,),
                1,
                repeat=1,
            )
        )
    include_counts = (100,) if quick else (100, 1000, 5000)
    for count in include_counts:
        include_file = temporary_directory / f"dynamic-include-{count}.php"
        include_file.write_text(dynamic_include_source(count))
        result.append(
            Benchmark(
                f"dynamic_include_{count}",
                "scaling",
                dynamic_include_root(count),
                f"dynamic_include_root_{count}",
                (str(include_file),),
                1,
                repeat=1,
            )
        )
    return tuple(result)


def run_php(
    php: Path,
    runner: str,
    benchmark: Benchmark,
    target: str,
    *,
    repeat: int | None = None,
) -> tuple[dict[str, Any], int]:
    with tempfile.NamedTemporaryFile(
        mode="w", encoding="utf-8", suffix=".php"
    ) as source_file:
        source_file.write(benchmark.source)
        source_file.flush()
        env = os.environ.copy()
        env.update(
            {
                "NATIVE_BENCH_SOURCE_FILE": source_file.name,
                "NATIVE_BENCH_ARGUMENTS": json.dumps(benchmark.arguments),
                "NATIVE_BENCH_FUNCTION": benchmark.function,
                "NATIVE_BENCH_FILENAME": f"benchmark-{benchmark.name}.php",
                "NATIVE_BENCH_TARGET": target,
                "NATIVE_BENCH_REPEAT": str(
                    benchmark.repeat if repeat is None else repeat
                ),
            }
        )
        command = [str(php), "-n"]
        for setting in benchmark.ini:
            command.extend(("-d", setting))
        command.extend(("-r", runner))
        completed = subprocess.run(
            command,
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
    if completed.returncode != 0:
        raise RuntimeError(
            f"{benchmark.name}: {php} exited {completed.returncode}: "
            f"stdout={completed.stdout[-1000:]!r}; "
            f"stderr={completed.stderr[-1000:]!r}"
        )
    try:
        data = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        raise RuntimeError(
            f"{benchmark.name}: invalid JSON from {php}: "
            f"{completed.stdout[-1000:]!r}; stderr={completed.stderr[-1000:]!r}"
        ) from error
    return data, completed.returncode


def product_source(benchmark: Benchmark) -> str:
    return (
        benchmark.source.rstrip()
        + "\n"
        + PRODUCT_RUNNER.replace("__NATIVE_BENCH_FUNCTION__", benchmark.function)
    )


def product_cold_benchmark(benchmark: Benchmark) -> Benchmark:
    """Return a first-call probe that does not include the timed workload."""
    arguments = benchmark.arguments
    if (
        benchmark.suite in {"direct", "hot"}
        and arguments
        and isinstance(arguments[0], int)
    ):
        arguments = (1, *arguments[1:])
    return replace(benchmark, arguments=arguments, repeat=1)


def benchmark_environment(benchmark: Benchmark, target: str) -> dict[str, str]:
    env = os.environ.copy()
    env.update(
        {
            "NATIVE_BENCH_ARGUMENTS": json.dumps(benchmark.arguments),
            "NATIVE_BENCH_FUNCTION": benchmark.function,
            "NATIVE_BENCH_FILENAME": f"benchmark-{benchmark.name}.php",
            "NATIVE_BENCH_TARGET": target,
            "NATIVE_BENCH_REPEAT": str(benchmark.repeat),
        }
    )
    return env


def parse_product_output(
    benchmark: Benchmark, stdout: str, stderr: str
) -> dict[str, Any]:
    body = stdout
    if "\r\n\r\n" in body:
        body = body.split("\r\n\r\n", 1)[1]
    elif "\n\n" in body:
        body = body.split("\n\n", 1)[1]
    try:
        return json.loads(body)
    except json.JSONDecodeError as error:
        raise RuntimeError(
            f"{benchmark.name}: invalid product JSON: "
            f"{body[-1000:]!r}; stderr={stderr[-1000:]!r}"
        ) from error


def run_product_cli(
    php: Path,
    source: Path,
    benchmark: Benchmark,
    target: str,
    opcache: bool,
    file_cache: Path,
) -> dict[str, Any]:
    command = [str(php), "-n"]
    if opcache:
        command.extend(
            (
                "-d",
                "opcache.enable_cli=1",
                "-d",
                f"opcache.file_cache={file_cache}",
                "-d",
                "opcache.file_cache_only=0",
                "-d",
                "opcache.validate_timestamps=0",
                "-d",
                "opcache.file_update_protection=0",
            )
        )
    else:
        command.extend(("-d", "opcache.enable_cli=0"))
    for setting in benchmark.ini:
        command.extend(("-d", setting))
    command.append(str(source))
    started = time.perf_counter_ns()
    completed = subprocess.run(
        command,
        env=benchmark_environment(benchmark, target),
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    wall_ns = time.perf_counter_ns() - started
    if completed.returncode != 0:
        raise RuntimeError(
            f"{benchmark.name}: {php} exited {completed.returncode}: "
            f"{completed.stderr.strip()}"
        )
    data = parse_product_output(benchmark, completed.stdout, completed.stderr)
    data["wall_ns"] = wall_ns
    return data


class FpmPool:
    def __init__(
        self,
        fpm: Path,
        cgi_fcgi: Path,
        directory: Path,
        opcache: bool,
    ) -> None:
        self.fpm = fpm
        self.cgi_fcgi = cgi_fcgi
        self.directory = directory
        self.opcache = opcache
        self.socket = directory / "benchmark.sock"
        self.config = directory / "php-fpm.conf"
        self.process: subprocess.Popen[str] | None = None

    def __enter__(self) -> FpmPool:
        self.config.write_text(
            "[global]\n"
            f"pid = {self.directory / 'php-fpm.pid'}\n"
            f"error_log = {self.directory / 'php-fpm.log'}\n"
            "daemonize = no\n"
            "[benchmark]\n"
            f"listen = {self.socket}\n"
            "listen.mode = 0600\n"
            "pm = static\n"
            "pm.max_children = 1\n"
            "pm.max_requests = 0\n"
            "clear_env = no\n"
            "catch_workers_output = yes\n",
            encoding="utf-8",
        )
        command = [
            str(self.fpm),
            "-n",
            "-y",
            str(self.config),
            "-F",
            "-O",
            "-d",
            f"opcache.enable={int(self.opcache)}",
            "-d",
            "opcache.validate_timestamps=0",
            "-d",
            "opcache.file_update_protection=0",
        ]
        self.process = subprocess.Popen(
            command,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        deadline = time.monotonic() + 15
        while not self.socket.exists():
            if self.process.poll() is not None:
                stdout, stderr = self.process.communicate()
                raise RuntimeError(
                    f"php-fpm exited {self.process.returncode}: {stdout}{stderr}"
                )
            if time.monotonic() >= deadline:
                self.close()
                raise RuntimeError("php-fpm did not create its socket")
            time.sleep(0.01)
        return self

    def request(
        self,
        source: Path,
        benchmark: Benchmark,
        target: str,
    ) -> dict[str, Any]:
        env = benchmark_environment(benchmark, target)
        env.update(
            {
                "SCRIPT_FILENAME": str(source),
                "SCRIPT_NAME": "/" + source.name,
                "REQUEST_METHOD": "GET",
                "REQUEST_URI": "/" + source.name,
                "QUERY_STRING": "",
                "SERVER_PROTOCOL": "HTTP/1.1",
                "GATEWAY_INTERFACE": "CGI/1.1",
                "SERVER_SOFTWARE": "native-benchmark",
                "REMOTE_ADDR": "127.0.0.1",
                "SERVER_ADDR": "127.0.0.1",
                "SERVER_PORT": "9000",
                "CONTENT_LENGTH": "0",
            }
        )
        started = time.perf_counter_ns()
        completed = subprocess.run(
            [
                str(self.cgi_fcgi),
                "-bind",
                "-connect",
                str(self.socket),
            ],
            env=env,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        wall_ns = time.perf_counter_ns() - started
        if completed.returncode != 0:
            raise RuntimeError(
                f"{benchmark.name}: cgi-fcgi exited "
                f"{completed.returncode}: {completed.stderr.strip()}"
            )
        data = parse_product_output(benchmark, completed.stdout, completed.stderr)
        data["wall_ns"] = wall_ns
        return data

    def close(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
        try:
            self.process.communicate(timeout=10)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.communicate()
        self.process = None

    def __exit__(self, *args: object) -> None:
        self.close()


@dataclass(frozen=True)
class ProductRole:
    name: str
    php: Path
    file_cache: Path
    fpm_pool: FpmPool | None


def measure_product_roles(
    roles: tuple[ProductRole, ...],
    source: Path,
    benchmark: Benchmark,
    target: str,
    samples: int,
    opcache: bool,
    *,
    optional_roles: frozenset[str] = frozenset(),
) -> tuple[
    dict[
        str,
        tuple[
            list[dict[str, Any]],
            list[dict[str, Any]],
            dict[str, Any] | None,
            list[dict[str, Any]] | None,
        ],
    ],
    dict[str, str],
]:
    cold_benchmark = product_cold_benchmark(benchmark)
    state: dict[str, dict[str, Any]] = {
        role.name: {
            "measured": [],
            "cold": [],
            "prime": None,
            "warm_probe": [],
        }
        for role in roles
    }
    errors: dict[str, str] = {}

    def invoke(
        role: ProductRole,
        path: Path = source,
        descriptor: Benchmark = benchmark,
    ) -> dict[str, Any]:
        data = (
            role.fpm_pool.request(path, descriptor, target)
            if role.fpm_pool is not None
            else run_product_cli(
                role.php,
                path,
                descriptor,
                target,
                opcache,
                role.file_cache,
            )
        )
        if data.get("status") != "returned":
            raise RuntimeError(
                f"{benchmark.name}: {role.name} product executor returned "
                f"{data.get('status')}"
            )
        return data

    def optional_invoke(
        role: ProductRole,
        path: Path = source,
        descriptor: Benchmark = benchmark,
    ) -> dict[str, Any] | None:
        if role.name in errors:
            return None
        try:
            return invoke(role, path, descriptor)
        except RuntimeError as error:
            if role.name not in optional_roles:
                raise
            errors[role.name] = str(error)
            state[role.name]["measured"] = []
            state[role.name]["cold"] = []
            state[role.name]["prime"] = None
            state[role.name]["warm_probe"] = []
            return None

    def rotated(index: int) -> tuple[ProductRole, ...]:
        if not roles:
            return ()
        offset = index % len(roles)
        return roles[offset:] + roles[:offset]

    # Keep the fresh-process latency comparison independent of role order.  The
    # first CLI invocation also pays one-time loader and filesystem-cache costs;
    # without an unmeasured invocation the first role pays those costs while the
    # other roles inherit the warmed host state.
    for role in roles:
        if role.fpm_pool is None:
            optional_invoke(role, descriptor=cold_benchmark)
            if opcache and role.name not in errors:
                shutil.rmtree(role.file_cache)
                role.file_cache.mkdir()

    # Rotate role order for every sample so thermal and frequency drift cannot
    # systematically favor the baseline over the candidate (or vice versa).
    for index in range(samples):
        cold_source = source
        if opcache and any(role.fpm_pool is not None for role in roles):
            cold_source = source.with_name(
                f"{source.stem}-cold-{index}{source.suffix}"
            )
            shutil.copyfile(source, cold_source)
        for role in rotated(index):
            if role.name in errors:
                continue
            if opcache and role.fpm_pool is None:
                shutil.rmtree(role.file_cache)
                role.file_cache.mkdir()
            sample = optional_invoke(role, cold_source, cold_benchmark)
            if sample is not None:
                state[role.name]["cold"].append(sample)

    if opcache:
        for role in rotated(samples):
            if role.name in errors:
                continue
            if role.fpm_pool is None:
                shutil.rmtree(role.file_cache)
                role.file_cache.mkdir()
            state[role.name]["prime"] = optional_invoke(
                role, descriptor=cold_benchmark
            )
        for index in range(samples):
            for role in rotated(samples + 1 + index):
                sample = optional_invoke(role, descriptor=cold_benchmark)
                if sample is not None:
                    state[role.name]["warm_probe"].append(sample)

    for index in range(samples):
        for role in rotated((2 * samples) + 1 + index):
            sample = optional_invoke(role)
            if sample is not None:
                state[role.name]["measured"].append(sample)

    result = {
        role.name: (
            state[role.name]["measured"],
            state[role.name]["cold"],
            state[role.name]["prime"],
            state[role.name]["warm_probe"] if opcache else None,
        )
        for role in roles
        if role.name not in errors
    }
    return result, errors


def measure_product(
    php: Path,
    source: Path,
    benchmark: Benchmark,
    target: str,
    samples: int,
    opcache: bool,
    file_cache: Path,
    fpm_pool: FpmPool | None,
) -> tuple[
    list[dict[str, Any]],
    list[dict[str, Any]],
    dict[str, Any] | None,
    list[dict[str, Any]] | None,
]:
    measurements, errors = measure_product_roles(
        (ProductRole("candidate", php, file_cache, fpm_pool),),
        source,
        benchmark,
        target,
        samples,
        opcache,
    )
    if errors:
        raise RuntimeError(next(iter(errors.values())))
    return measurements["candidate"]


def validate_product_opcache_samples(
    benchmark: Benchmark,
    role: str,
    samples: list[dict[str, Any]],
    expected_enabled: bool,
    require_hit: bool,
) -> None:
    """Fail unless every product sample exposes the requested OPcache state."""
    if not samples:
        raise RuntimeError(f"{benchmark.name}: {role} recorded no OPcache samples")
    for index, sample in enumerate(samples, 1):
        enabled = sample.get("opcache_enabled")
        if enabled is not expected_enabled:
            raise RuntimeError(
                f"{benchmark.name}: {role} sample {index} reported "
                f"opcache_enabled={enabled!r}, expected {expected_enabled!r}"
            )
        metrics = {}
        for metric in (
            "opcache_hits",
            "opcache_misses",
            "opcache_cached_scripts",
        ):
            value = sample.get(metric)
            if isinstance(value, bool) or not isinstance(value, int) or value < 0:
                raise RuntimeError(
                    f"{benchmark.name}: {role} sample {index} has invalid "
                    f"{metric}={value!r}"
                )
            metrics[metric] = value
        if not expected_enabled and any(metrics.values()):
            raise RuntimeError(
                f"{benchmark.name}: {role} sample {index} reported OPcache "
                f"statistics while disabled: {metrics!r}"
            )
        if expected_enabled and require_hit:
            if metrics["opcache_cached_scripts"] < 1:
                raise RuntimeError(
                    f"{benchmark.name}: {role} sample {index} cached no scripts"
                )
            if metrics["opcache_hits"] < 1:
                raise RuntimeError(
                    f"{benchmark.name}: {role} sample {index} recorded no OPcache hit"
                )


def percentile(values: Iterable[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1)
    return ordered[index]


def percentile_metric(
    samples: list[dict[str, Any]], path: tuple[str, ...], fraction: float
) -> float | None:
    values = []
    for sample in samples:
        value: Any = sample
        for key in path:
            if not isinstance(value, dict) or key not in value:
                value = None
                break
            value = value[key]
        if isinstance(value, (int, float)):
            values.append(float(value))
    return percentile(values, fraction) if values else None


def median_metric(samples: list[dict[str, Any]], path: tuple[str, ...]) -> float | None:
    values = []
    for sample in samples:
        value: Any = sample
        for key in path:
            if not isinstance(value, dict) or key not in value:
                value = None
                break
            value = value[key]
        if isinstance(value, (int, float)):
            values.append(float(value))
    return statistics.median(values) if values else None


def measure(
    php: Path,
    benchmark: Benchmark,
    target: str,
    samples: int,
    runner: str,
) -> list[dict[str, Any]]:
    measured = []
    for _ in range(samples):
        data, _ = run_php(php, runner, benchmark, target)
        if data.get("status") not in {"accepted", "returned"}:
            raise RuntimeError(
                f"{benchmark.name}: {php} returned "
                f"{data.get('status')} at {data.get('phase')}"
            )
        for counter in (
            "vm_handler_calls",
            "execute_ex_calls",
            "opline_handler_calls",
        ):
            if data.get(counter) not in (None, 0):
                raise RuntimeError(f"{benchmark.name}: {counter}={data[counter]}")
        measured.append(data)
    return measured


def summarize(
    benchmark: Benchmark,
    candidate: list[dict[str, Any]],
    baseline: list[dict[str, Any]] | None,
    reference: list[dict[str, Any]] | None,
    cold: list[dict[str, Any]] | None = None,
    prime: dict[str, Any] | None = None,
    baseline_cold: list[dict[str, Any]] | None = None,
    warm_probe: list[dict[str, Any]] | None = None,
    *,
    mode: str = "diagnostic",
    opcache: bool = False,
) -> dict[str, Any]:
    if mode not in {"diagnostic", "product-cli", "product-fpm"}:
        raise ValueError(f"unsupported performance measurement mode: {mode}")
    candidate_results = [sample.get("return_value") for sample in candidate]
    if any(result != candidate_results[0] for result in candidate_results[1:]):
        raise RuntimeError(
            f"{benchmark.name}: candidate return value is nondeterministic"
        )
    baseline_error = None
    if baseline:
        baseline_results = [sample.get("return_value") for sample in baseline]
        if any(result != baseline_results[0] for result in baseline_results[1:]):
            baseline_error = (
                f"{benchmark.name}: baseline return value is nondeterministic"
            )
        elif baseline_results[0] != candidate_results[0]:
            baseline_error = (
                f"{benchmark.name}: candidate returned "
                f"{candidate_results[0]!r}, baseline returned "
                f"{baseline_results[0]!r}"
            )
        if baseline_error is not None:
            baseline = None
            baseline_cold = None
    if reference:
        reference_results = [sample.get("return_value") for sample in reference]
        if any(result != reference_results[0] for result in reference_results[1:]):
            raise RuntimeError(
                f"{benchmark.name}: reference return value is nondeterministic"
            )
        if reference_results[0] != candidate_results[0]:
            raise RuntimeError(
                f"{benchmark.name}: candidate returned "
                f"{candidate_results[0]!r}, reference returned "
                f"{reference_results[0]!r}"
            )

    candidate_bridge = median_metric(candidate, ("bridge_ns",))
    candidate_product_execute = median_metric(candidate, ("execute_ns",))
    candidate_total = (
        candidate_bridge
        if candidate_bridge is not None
        else candidate_product_execute or 0.0
    )
    candidate_comparable = candidate_total / benchmark.repeat
    candidate_execute = median_metric(candidate, ("performance", "last_execute_ns"))
    if candidate_execute is None:
        candidate_execute = candidate_total / benchmark.repeat
    candidate_cold_samples = cold if cold is not None else candidate
    candidate_compile_values = [
        float(sample["performance"]["compile_ns"])
        for sample in candidate_cold_samples
        if isinstance(sample.get("performance"), dict)
        and isinstance(sample["performance"].get("compile_ns"), (int, float))
    ]
    candidate_compile_p95 = (
        percentile(candidate_compile_values, 0.95) if candidate_compile_values else None
    )
    candidate_cold_wall_values = [
        float(sample["wall_ns"])
        for sample in candidate_cold_samples
        if isinstance(sample.get("wall_ns"), (int, float))
    ]
    candidate_cold_wall_p95 = (
        percentile(candidate_cold_wall_values, 0.95)
        if candidate_cold_wall_values
        else None
    )
    record: dict[str, Any] = {
        "suite": benchmark.suite,
        "case": benchmark.name,
        "operations": benchmark.operations,
        "candidate_ns_per_operation": (candidate_execute / benchmark.operations),
        "candidate_comparable_ns_per_operation": (
            candidate_comparable / benchmark.operations
        ),
        "candidate_bridge_ns": candidate_bridge,
        "candidate_execute_ns": candidate_product_execute,
        "candidate_peak_rss_bytes": percentile_metric(
            candidate, ("peak_rss_bytes",), 0.95
        ),
        "peak_rss_definition": "p95 of per-sample process peak RSS",
    }
    if baseline_error is not None:
        record["baseline_error"] = baseline_error
    if mode == "diagnostic":
        record["native_compile_p95_ns"] = candidate_compile_p95
    else:
        record["product_cold_compile_latency_p95_ns"] = candidate_cold_wall_p95
        if mode == "product-cli":
            record["product_cold_compile_latency_definition"] = (
                "end-to-end wall_ns for the first request in a fresh CLI "
                "process with an empty OPcache file cache after one "
                "unmeasured loader warmup"
                if opcache
                else "end-to-end wall_ns for a request in a fresh CLI process "
                "with OPcache disabled after one unmeasured loader warmup"
            )
        else:
            record["product_cold_compile_latency_definition"] = (
                "end-to-end wall_ns for the first request to a fresh script "
                "path in one persistent FPM worker"
                if opcache
                else "end-to-end request wall_ns in one persistent FPM worker "
                "with OPcache disabled"
            )
    if cold is not None:
        cold_wall = median_metric(cold, ("wall_ns",))
        record["cold_wall_ns"] = cold_wall
        record["cold_opcache_hits"] = (
            prime.get("opcache_hits") if prime is not None else None
        )
        record["cold_opcache_misses"] = (
            prime.get("opcache_misses") if prime is not None else None
        )
        if warm_probe is not None:
            warm_wall = median_metric(warm_probe, ("wall_ns",))
            record.update(
                {
                    "warm_wall_ns": warm_wall,
                    "warm_vs_cold": (
                        float(cold_wall) / warm_wall
                        if isinstance(cold_wall, (int, float))
                        and warm_wall is not None
                        and warm_wall > 0
                        else None
                    ),
                    "warm_opcache_hits": median_metric(
                        warm_probe, ("opcache_hits",)
                    ),
                    "warm_opcache_misses": median_metric(
                        warm_probe, ("opcache_misses",)
                    ),
                    "warm_opcache_cached_scripts": median_metric(
                        warm_probe, ("opcache_cached_scripts",)
                    ),
                }
            )
    performance = (
        next(
            (
                sample["performance"]
                for sample in candidate
                if isinstance(sample.get("performance"), dict)
            ),
            None,
        )
        if mode == "diagnostic"
        else None
    )
    if performance is not None:
        for key in (
            "registered_codeunits",
            "compiled_codeunits",
            "ready_codeunits",
            "published_components",
            "ssa_ns",
            "lowering_ns",
            "codegen_ns",
            "publish_ns",
            "native_code_bytes",
            "runtime_helper_sites",
            "source_opline_decode_sites",
            "guard_sites",
            "slow_path_sites",
            "direct_call_sites",
            "direct_leaf_scalar_sites",
            "direct_typed_body_sites",
            "direct_call_frame_bytes",
            "inner_call_runtime_helper_calls",
            "inner_call_heap_allocations",
            "inner_call_catcher_boundaries",
        ):
            record[key] = performance.get(key)
    if baseline:
        baseline_bridge = median_metric(baseline, ("bridge_ns",))
        baseline_product_execute = median_metric(baseline, ("execute_ns",))
        baseline_total = (
            baseline_bridge
            if baseline_bridge is not None
            else baseline_product_execute or 0.0
        )
        baseline_execute = baseline_total / benchmark.repeat
        record["baseline_ns_per_operation"] = baseline_execute / benchmark.operations
        record["baseline_bridge_ns"] = baseline_bridge
        record["baseline_execute_ns"] = baseline_product_execute
        record["baseline_peak_rss_bytes"] = percentile_metric(
            baseline, ("peak_rss_bytes",), 0.95
        )
        baseline_cold_samples = baseline_cold if baseline_cold is not None else baseline
        baseline_compile_values = [
            float(sample["performance"]["compile_ns"])
            for sample in baseline_cold_samples
            if isinstance(sample.get("performance"), dict)
            and isinstance(sample["performance"].get("compile_ns"), (int, float))
        ]
        baseline_compile_p95 = (
            percentile(baseline_compile_values, 0.95)
            if baseline_compile_values
            else None
        )
        baseline_cold_wall_values = [
            float(sample["wall_ns"])
            for sample in baseline_cold_samples
            if isinstance(sample.get("wall_ns"), (int, float))
        ]
        baseline_cold_wall_p95 = (
            percentile(baseline_cold_wall_values, 0.95)
            if baseline_cold_wall_values
            else None
        )
        if mode == "diagnostic":
            record["baseline_native_compile_p95_ns"] = baseline_compile_p95
        else:
            record["baseline_product_cold_compile_latency_p95_ns"] = (
                baseline_cold_wall_p95
            )
        if mode == "diagnostic" and (
            candidate_compile_p95 is not None
            and baseline_compile_p95 is not None
            and candidate_compile_p95 > 0
            and baseline_compile_p95 > 0
        ):
            record["cold_measurement_definition"] = (
                "native compiler compile_ns reported by a fresh invocation"
            )
            record["cold_compile_p95_ratio"] = (
                candidate_compile_p95 / baseline_compile_p95
            )
        elif mode != "diagnostic" and (
            candidate_cold_wall_p95 is not None
            and baseline_cold_wall_p95 is not None
            and candidate_cold_wall_p95 > 0
            and baseline_cold_wall_p95 > 0
        ):
            record["product_cold_compile_latency_p95_ratio"] = (
                candidate_cold_wall_p95 / baseline_cold_wall_p95
            )
        candidate_rss = record["candidate_peak_rss_bytes"]
        baseline_rss = record["baseline_peak_rss_bytes"]
        record["peak_rss_ratio"] = (
            candidate_rss / baseline_rss
            if isinstance(candidate_rss, (int, float))
            and isinstance(baseline_rss, (int, float))
            and baseline_rss > 0
            else None
        )
        record["speedup"] = (
            baseline_execute / candidate_comparable if candidate_comparable > 0 else 0.0
        )
    if reference:
        reference_execute = median_metric(reference, ("execute_ns",)) or 0.0
        reference_per_operation = (
            reference_execute / benchmark.repeat / benchmark.operations
        )
        record["reference_ns_per_operation"] = reference_per_operation
        record["reference_peak_rss_bytes"] = percentile_metric(
            reference, ("peak_rss_bytes",), 0.95
        )
        record["candidate_vs_reference"] = (
            reference_per_operation / record["candidate_ns_per_operation"]
            if record["candidate_ns_per_operation"] > 0
            else 0.0
        )
    return record


def geometric_mean(values: Iterable[float]) -> float:
    positive = [value for value in values if value > 0]
    return math.exp(sum(math.log(value) for value in positive) / len(positive))


def v1_product_contract_failures(
    records: list[dict[str, Any]], summary: dict[str, Any]
) -> list[str]:
    """Return failures for the product-mode V1 performance contract."""
    failures = []
    direct_scalar = next(
        (record for record in records if record["case"] == "scalar_return"),
        None,
    )
    if direct_scalar is not None:
        if summary.get("direct_scalar_speedup", 0) < V1_MIN_BASELINE_SPEEDUP:
            failures.append("direct scalar call regresses by more than 10%")
        if (
            summary.get("direct_scalar_vs_reference", 0)
            < V1_MIN_DIRECT_REFERENCE_SPEEDUP
        ):
            failures.append("direct scalar call is less than 2.0x the reference VM")

    hot_records = [record for record in records if record["suite"] == "hot"]
    if hot_records:
        if summary.get("hot_geomean_speedup", 0) < V1_MIN_BASELINE_SPEEDUP:
            failures.append("hot corpus regresses by more than 10%")
        if summary.get("hot_geomean_vs_reference", 0) < V1_MIN_HOT_REFERENCE_SPEEDUP:
            failures.append("hot corpus geometric mean is below 1.25x the reference VM")

    comparable = [
        record for record in records if record["suite"] in {"direct", "hot", "scaling"}
    ]
    missing_cold_measurement = [
        record["case"]
        for record in comparable
        if "baseline_error" not in record
        if not isinstance(
            record.get("product_cold_compile_latency_p95_ratio"), (int, float)
        )
    ]
    if missing_cold_measurement:
        failures.append(
            "product cold compile latency p95 comparison missing for: "
            + ", ".join(missing_cold_measurement)
        )
    cold_compile_regressions = [
        record["case"]
        for record in comparable
        if isinstance(
            record.get("product_cold_compile_latency_p95_ratio"), (int, float)
        )
        and float(record["product_cold_compile_latency_p95_ratio"])
        > V1_MAX_RESOURCE_RATIO
    ]
    if cold_compile_regressions:
        failures.append(
            "product cold compile latency p95 regresses by more than 20% for: "
            + ", ".join(cold_compile_regressions)
        )

    missing_rss = [
        record["case"]
        for record in comparable
        if "baseline_error" not in record
        if not isinstance(record.get("peak_rss_ratio"), (int, float))
    ]
    if missing_rss:
        failures.append("peak RSS comparison missing for: " + ", ".join(missing_rss))
    peak_rss_regressions = [
        record["case"]
        for record in comparable
        if isinstance(record.get("peak_rss_ratio"), (int, float))
        and float(record["peak_rss_ratio"]) > V1_MAX_RESOURCE_RATIO
    ]
    if peak_rss_regressions:
        failures.append(
            "peak RSS regresses by more than 20% for: "
            + ", ".join(peak_rss_regressions)
        )
    return failures


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--reference", type=Path)
    parser.add_argument(
        "--mode",
        choices=("diagnostic", "product-cli", "product-fpm"),
        default="diagnostic",
        help=(
            "diagnostic preserves the W12 bridge measurement; product modes "
            "execute the benchmark as ordinary userland through the global "
            "native executor"
        ),
    )
    parser.add_argument("--opcache", choices=("off", "on"), default="off")
    parser.add_argument(
        "--fpm",
        type=Path,
        help="php-fpm binary; inferred beside the candidate CLI binary",
    )
    parser.add_argument(
        "--baseline-fpm",
        type=Path,
        help="baseline php-fpm binary; inferred beside the baseline CLI binary",
    )
    parser.add_argument(
        "--reference-fpm",
        type=Path,
        help=("reference php-fpm binary; inferred beside the reference CLI binary"),
    )
    parser.add_argument(
        "--cgi-fcgi",
        type=Path,
        help="cgi-fcgi client used for persistent FPM requests",
    )
    parser.add_argument(
        "--w12-baseline",
        action="store_true",
        help="enforce the W13 retention limits against the exact W12 binary",
    )
    parser.add_argument(
        "--w13-baseline",
        action="store_true",
        help="enforce the W14 retention and cutover limits against W13",
    )
    parser.add_argument("--target", choices=tuple(TARGET_BY_HOST.values()))
    parser.add_argument(
        "--suite",
        choices=("all", "direct", "hot", "scaling"),
        default="all",
    )
    parser.add_argument(
        "--case",
        action="append",
        dest="cases",
        help="run only the named benchmark case; may be repeated",
    )
    parser.add_argument("--samples", type=int, default=DEFAULT_SAMPLES)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--enforce", action="store_true")
    return parser.parse_args()


def verify_target_host(target: str) -> None:
    expected = next(
        (host for host, identifier in TARGET_BY_HOST.items() if identifier == target),
        None,
    )
    if expected is None:
        raise UsageError(f"unknown benchmark target: {target}")
    host = (platform.system(), platform.machine())
    if host != expected:
        raise PrerequisiteError(
            f"{target} requires {expected[0]}/{expected[1]}; "
            f"host is {host[0]}/{host[1]}"
        )
    if target == "darwin-arm64-dev":
        translated = subprocess.run(
            ["sysctl", "-in", "sysctl.proc_translated"],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            check=False,
        )
        value = translated.stdout.strip() if translated.returncode == 0 else "0"
        if value != "0":
            raise PrerequisiteError(
                "darwin-arm64-dev cannot be benchmarked through Rosetta"
            )


def verify_executable(path: Path, role: str) -> None:
    if not path.is_file() or not os.access(path, os.X_OK):
        raise PrerequisiteError(f"{role} PHP binary is unavailable: {path}")


def main() -> int:
    args = parse_args()
    host = (platform.system(), platform.machine())
    target = args.target or TARGET_BY_HOST.get(host)
    if target is None:
        raise PrerequisiteError(
            f"unsupported benchmark host {host[0]}/{host[1]}"
        )
    verify_target_host(target)
    if args.samples < 1:
        raise UsageError("--samples must be positive")
    verify_executable(args.candidate, "candidate")
    for role, binary in (("baseline", args.baseline), ("reference", args.reference)):
        if binary is not None:
            verify_executable(binary, role)
    if args.mode == "diagnostic" and args.opcache != "off":
        raise UsageError("--opcache applies only to product-cli and product-fpm")
    if args.w12_baseline and args.w13_baseline:
        raise UsageError("--w12-baseline and --w13-baseline are exclusive")
    opcache = args.opcache == "on"
    fpm = args.fpm
    baseline_fpm = args.baseline_fpm
    reference_fpm = args.reference_fpm
    cgi_fcgi = args.cgi_fcgi
    if args.mode == "product-fpm":
        if fpm is None:
            fpm = args.candidate.parent.parent / "fpm" / "php-fpm"
        if not fpm.is_file() or not os.access(fpm, os.X_OK):
            raise PrerequisiteError(f"candidate php-fpm is unavailable: {fpm}")
        if args.baseline is not None:
            if baseline_fpm is None:
                baseline_fpm = args.baseline.parent.parent / "fpm" / "php-fpm"
            if not baseline_fpm.is_file() or not os.access(baseline_fpm, os.X_OK):
                raise PrerequisiteError(
                    f"baseline php-fpm is unavailable: {baseline_fpm}"
                )
        if args.reference is not None:
            if reference_fpm is None:
                reference_fpm = args.reference.parent.parent / "fpm" / "php-fpm"
            if not reference_fpm.is_file() or not os.access(
                reference_fpm, os.X_OK
            ):
                raise PrerequisiteError(
                    f"reference php-fpm is unavailable: {reference_fpm}"
                )
        if cgi_fcgi is None:
            located = shutil.which("cgi-fcgi")
            if located is not None:
                cgi_fcgi = Path(located)
        if (
            cgi_fcgi is None
            or not cgi_fcgi.is_file()
            or not os.access(cgi_fcgi, os.X_OK)
        ):
            raise PrerequisiteError(
                "product-fpm requires executable --cgi-fcgi or cgi-fcgi on PATH"
            )

    direct_iterations = 2_000 if args.quick else 200_000
    hot_iterations = 500 if args.quick else 50_000
    with tempfile.TemporaryDirectory(prefix="php-native-benchmark-") as temp:
        include_file = Path(temp) / "include-once.php"
        include_file.write_text(
            "<?php function included_once_value(): int { return 1; }\n"
        )
        benchmarks: tuple[Benchmark, ...] = ()
        if args.suite in {"all", "direct"}:
            benchmarks += direct_benchmarks(direct_iterations)
        if args.suite in {"all", "hot"}:
            benchmarks += hot_benchmarks(hot_iterations, include_file)
        if args.suite in {"all", "scaling"}:
            benchmarks += scaling_benchmarks(args.quick, Path(temp))
        if args.cases:
            selected = set(args.cases)
            benchmarks = tuple(
                benchmark for benchmark in benchmarks if benchmark.name in selected
            )
            missing = selected.difference(benchmark.name for benchmark in benchmarks)
            if missing:
                raise UsageError(
                    "unknown benchmark cases: " + ", ".join(sorted(missing))
                )

        file_cache = Path(temp) / "opcache-file-cache"
        file_cache.mkdir()
        baseline_file_cache = Path(temp) / "baseline-opcache-file-cache"
        baseline_file_cache.mkdir()
        reference_file_cache = Path(temp) / "reference-opcache-file-cache"
        reference_file_cache.mkdir()
        product_sources: dict[str, Path] = {}
        if args.mode != "diagnostic":
            for benchmark in benchmarks:
                source = Path(temp) / f"product-{benchmark.name}.php"
                source.write_text(product_source(benchmark), encoding="utf-8")
                product_sources[benchmark.name] = source

        records = []

        def run_benchmarks(
            fpm_pool: FpmPool | None,
            baseline_fpm_pool: FpmPool | None,
            reference_fpm_pool: FpmPool | None,
        ) -> None:
            for benchmark in benchmarks:
                cold = None
                prime = None
                baseline_error = None
                baseline_cold = None
                if args.mode == "diagnostic":
                    candidate = measure(
                        args.candidate,
                        benchmark,
                        target,
                        args.samples,
                        CANDIDATE_RUNNER,
                    )
                    if args.baseline is None:
                        baseline = None
                    else:
                        try:
                            baseline = measure(
                                args.baseline,
                                benchmark,
                                target,
                                args.samples,
                                CANDIDATE_RUNNER,
                            )
                        except RuntimeError as error:
                            baseline = None
                            baseline_error = str(error)
                    if args.reference is None:
                        reference = None
                    else:
                        reference = measure(
                            args.reference,
                            benchmark,
                            target,
                            args.samples,
                            REFERENCE_RUNNER,
                        )
                else:
                    roles = [
                        ProductRole(
                            "candidate", args.candidate, file_cache, fpm_pool
                        )
                    ]
                    if args.baseline is not None:
                        roles.append(
                            ProductRole(
                                "baseline",
                                args.baseline,
                                baseline_file_cache,
                                baseline_fpm_pool,
                            )
                        )
                    if args.reference is not None:
                        roles.append(
                            ProductRole(
                                "reference",
                                args.reference,
                                reference_file_cache,
                                reference_fpm_pool,
                            )
                        )
                    measurements, errors = measure_product_roles(
                        tuple(roles),
                        product_sources[benchmark.name],
                        benchmark,
                        target,
                        args.samples,
                        opcache,
                        optional_roles=frozenset({"baseline"}),
                    )
                    candidate, cold, prime, warm_probe = measurements["candidate"]
                    baseline_error = errors.get("baseline")
                    if "baseline" in measurements:
                        baseline, baseline_cold, _, baseline_warm_probe = (
                            measurements["baseline"]
                        )
                    else:
                        baseline = None
                        baseline_warm_probe = None
                    reference_cold = None
                    if "reference" in measurements:
                        reference, reference_cold, _, reference_warm_probe = (
                            measurements["reference"]
                        )
                    else:
                        reference = None
                        reference_warm_probe = None
                    for role_name, role_samples, role_cold, role_warm_probe in (
                        ("candidate", candidate, cold, warm_probe),
                        (
                            "baseline",
                            baseline,
                            baseline_cold,
                            baseline_warm_probe,
                        ),
                        (
                            "reference",
                            reference,
                            reference_cold,
                            reference_warm_probe,
                        ),
                    ):
                        if role_samples is None:
                            continue
                        validate_product_opcache_samples(
                            benchmark,
                            f"{role_name} warm",
                            role_samples,
                            opcache,
                            require_hit=opcache,
                        )
                        assert role_cold is not None
                        validate_product_opcache_samples(
                            benchmark,
                            f"{role_name} cold",
                            role_cold,
                            opcache,
                            require_hit=False,
                        )
                        if role_warm_probe is not None:
                            validate_product_opcache_samples(
                                benchmark,
                                f"{role_name} warm probe",
                                role_warm_probe,
                                opcache,
                                require_hit=True,
                            )
                record = summarize(
                    benchmark,
                    candidate,
                    baseline,
                    reference,
                    cold,
                    prime,
                    baseline_cold,
                    mode=args.mode,
                    opcache=opcache,
                    warm_probe=(warm_probe if args.mode != "diagnostic" else None),
                )
                if baseline_error is not None:
                    record["baseline_error"] = baseline_error
                records.append(record)
                print(json.dumps(record, sort_keys=True), flush=True)

        if args.mode == "product-fpm":
            assert fpm is not None and cgi_fcgi is not None
            with ExitStack() as stack:
                fpm_directory = Path(temp) / "fpm"
                fpm_directory.mkdir()
                fpm_pool = stack.enter_context(
                    FpmPool(fpm, cgi_fcgi, fpm_directory, opcache)
                )
                baseline_pool = None
                if baseline_fpm is not None:
                    baseline_fpm_directory = Path(temp) / "baseline-fpm"
                    baseline_fpm_directory.mkdir()
                    baseline_pool = stack.enter_context(
                        FpmPool(
                            baseline_fpm,
                            cgi_fcgi,
                            baseline_fpm_directory,
                            opcache,
                        )
                    )
                reference_pool = None
                if reference_fpm is not None:
                    reference_fpm_directory = Path(temp) / "reference-fpm"
                    reference_fpm_directory.mkdir()
                    reference_pool = stack.enter_context(
                        FpmPool(
                            reference_fpm,
                            cgi_fcgi,
                            reference_fpm_directory,
                            opcache,
                        )
                    )
                run_benchmarks(fpm_pool, baseline_pool, reference_pool)
        else:
            run_benchmarks(None, None, None)

    summary: dict[str, Any] = {
        "target": target,
        "mode": args.mode,
        "opcache": args.opcache,
        "cases": len(records),
    }
    hot_speedups = [
        float(record["speedup"])
        for record in records
        if record["suite"] == "hot" and "speedup" in record
    ]
    if hot_speedups:
        summary["hot_geomean_speedup"] = geometric_mean(hot_speedups)
    hot_reference_speedups = [
        float(record["candidate_vs_reference"])
        for record in records
        if record["suite"] == "hot"
        and isinstance(record.get("candidate_vs_reference"), (int, float))
    ]
    if hot_reference_speedups:
        summary["hot_geomean_vs_reference"] = geometric_mean(hot_reference_speedups)
    warm_speedups = [
        float(record["warm_vs_cold"])
        for record in records
        if isinstance(record.get("warm_vs_cold"), (int, float))
    ]
    if warm_speedups:
        summary["warm_geomean_speedup"] = geometric_mean(warm_speedups)
    w14_cutover_cases = {
        "scalar_return",
        "call_in_loop",
        "cv_assignment_loop",
        "packed_array_read",
        "standard_property_cached_read",
    }
    w14_cutover_speedups = [
        float(record["speedup"])
        for record in records
        if record["case"] in w14_cutover_cases
        and isinstance(record.get("speedup"), (int, float))
    ]
    if w14_cutover_speedups:
        summary["w14_cutover_geomean_speedup"] = geometric_mean(w14_cutover_speedups)
    direct_scalar = next(
        (record for record in records if record["case"] == "scalar_return"),
        None,
    )
    if direct_scalar is not None:
        if "speedup" in direct_scalar:
            summary["direct_scalar_speedup"] = direct_scalar["speedup"]
        if "candidate_vs_reference" in direct_scalar:
            summary["direct_scalar_vs_reference"] = direct_scalar[
                "candidate_vs_reference"
            ]
    independent_1000 = next(
        (record for record in records if record["case"] == "independent_1000"),
        None,
    )
    if independent_1000 is not None:
        summary["independent_1000_compiled_codeunits"] = independent_1000.get(
            "compiled_codeunits"
        )
        if "speedup" in independent_1000:
            summary["independent_1000_speedup"] = independent_1000["speedup"]
    regressions = [
        record["case"]
        for record in records
        if "speedup" in record
        and record["suite"] in {"direct", "hot"}
        # An enabled observer deliberately takes the semantic observer path;
        # it is measured, but it is not an unobserved representative fast path.
        and record["case"] != "observer_enabled"
        and float(record["speedup"]) < (1 / 1.10)
    ]
    summary["regressions_over_10_percent"] = regressions
    cold_compile_regressions = [
        record["case"]
        for record in records
        if record["suite"] in {"direct", "hot", "scaling"}
        and isinstance(record.get("cold_compile_p95_ratio"), (int, float))
        and float(record["cold_compile_p95_ratio"]) > 1.20
    ]
    summary["cold_compile_regressions_over_20_percent"] = cold_compile_regressions
    product_cold_compile_regressions = [
        record["case"]
        for record in records
        if record["suite"] in {"direct", "hot", "scaling"}
        and isinstance(
            record.get("product_cold_compile_latency_p95_ratio"), (int, float)
        )
        and float(record["product_cold_compile_latency_p95_ratio"])
        > V1_MAX_RESOURCE_RATIO
    ]
    summary["product_cold_compile_latency_regressions_over_20_percent"] = (
        product_cold_compile_regressions
    )
    peak_rss_regressions = [
        record["case"]
        for record in records
        if record["suite"] in {"direct", "hot", "scaling"}
        and isinstance(record.get("peak_rss_ratio"), (int, float))
        and float(record["peak_rss_ratio"]) > 1.20
    ]
    summary["peak_rss_regressions_over_20_percent"] = peak_rss_regressions
    print(json.dumps({"summary": summary}, sort_keys=True))

    if not args.enforce:
        return 0
    failures = []
    product_mode = args.mode != "diagnostic"
    if args.baseline is None or args.reference is None:
        failures.append("--enforce requires --baseline and --reference")
    if direct_scalar is not None:
        if not product_mode:
            minimum = 0.95 if args.w12_baseline else 0.97 if args.w13_baseline else 3.0
            if summary.get("direct_scalar_speedup", 0) < minimum:
                failures.append(
                    "direct scalar call retains less than 95% of W12"
                    if args.w12_baseline
                    else "direct scalar call regresses by more than 3% from W13"
                    if args.w13_baseline
                    else "direct scalar call speedup is below 3.0x"
                )
    elif args.suite in {"all", "direct"} and not args.cases:
        failures.append("direct scalar benchmark was not executed")
    if hot_speedups:
        if not product_mode:
            minimum = 0.95 if args.w12_baseline else 0.97 if args.w13_baseline else 1.5
            if summary.get("hot_geomean_speedup", 0) < minimum:
                failures.append(
                    "hot corpus retains less than 95% of W12"
                    if args.w12_baseline
                    else "hot corpus regresses by more than 3% from W13"
                    if args.w13_baseline
                    else "hot corpus geometric mean speedup is below 1.5x"
                )
    elif args.suite in {"all", "hot"} and not args.cases:
        failures.append("hot corpus was not executed")
    if regressions:
        failures.append(
            "representative cases regress by more than 10%: " + ", ".join(regressions)
        )
    if product_mode:
        failures.extend(v1_product_contract_failures(records, summary))
    if opcache and warm_speedups:
        if summary.get("warm_geomean_speedup", 0) <= 1.0:
            failures.append(
                "warm OPcache/FPM requests are not faster than cold requests"
            )
    elif opcache and args.mode != "diagnostic":
        failures.append("no cold/warm OPcache samples were recorded")
    if args.mode == "product-fpm" and opcache:
        hit_growth = [
            record
            for record in records
            if isinstance(record.get("cold_opcache_hits"), (int, float))
            and isinstance(record.get("warm_opcache_hits"), (int, float))
            and float(record["warm_opcache_hits"]) > float(record["cold_opcache_hits"])
        ]
        if not hit_growth:
            failures.append("FPM requests did not produce an OPcache hit")
    if args.w13_baseline and w14_cutover_speedups:
        if summary.get("w14_cutover_geomean_speedup", 0) <= 1.0:
            failures.append("register-centered W14 cutover cases do not beat W13")
    if (
        independent_1000 is not None
        and args.mode == "diagnostic"
        and not args.w13_baseline
    ):
        compiled = independent_1000.get("compiled_codeunits")
        if compiled != V1_LAZY_CODEUNITS:
            failures.append(
                "1k/1-root+dependency compilation did not compile exactly two codeunits"
            )
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    return 0


def cli_main() -> int:
    try:
        return main()
    except UsageError as error:
        print(f"error: {error}", file=sys.stderr)
        return 2
    except PrerequisiteError as error:
        print(f"error: {error}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    raise SystemExit(cli_main())
