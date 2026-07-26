#!/usr/bin/env python3
"""Measure the native baseline without creating a benchmark ledger."""

from __future__ import annotations

import argparse
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
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


TARGET_BY_HOST = {
    ("Darwin", "arm64"): "darwin-arm64-dev",
    ("Linux", "x86_64"): "linux-amd64-prod",
}

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
                name, "direct", "<?php\n" + body, function,
                (iterations,), iterations, ini=ini,
            )
        )
    return tuple(result)


def hot_benchmarks(
    iterations: int, include_file: Path
) -> tuple[Benchmark, ...]:
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
            name, "hot", "<?php\n" + source, function,
            (iterations,), iterations,
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
        + "\nfunction scaling_root(int $v): int { return $v + 1; }\n"
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
        "<?php\n"
        + classes
        + "\nclass UsedClass {"
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
        "<?php\n"
        + functions
        + f"\nfunction dynamic_used_{count}(int $v): int {{"
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


def scaling_benchmarks(
    quick: bool, temporary_directory: Path
) -> tuple[Benchmark, ...]:
    independent_counts = (100, 1000) if quick else (100, 1000, 5000)
    scc_counts = (2, 10) if quick else (2, 10, 100)
    method_counts = (100,) if quick else (100, 1000)
    result = []
    for count in independent_counts:
        result.append(
            Benchmark(
                f"independent_{count}", "scaling", independent_source(count),
                "scaling_root", (1,), 1, repeat=1,
            )
        )
    for count in scc_counts:
        result.append(
            Benchmark(
                f"scc_{count}", "scaling", scc_source(count),
                "scc_root", (count,), count + 1, repeat=1,
            )
        )
    for count in method_counts:
        result.append(
            Benchmark(
                f"methods_{count}", "scaling", method_source(count),
                "method_scaling_root", (1,), 1, repeat=1,
            )
        )
    include_counts = (100,) if quick else (100, 1000, 5000)
    for count in include_counts:
        include_file = (
            temporary_directory / f"dynamic-include-{count}.php"
        )
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
        + PRODUCT_RUNNER.replace(
            "__NATIVE_BENCH_FUNCTION__", benchmark.function
        )
    )


def benchmark_environment(
    benchmark: Benchmark, target: str
) -> dict[str, str]:
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
                "-d", "opcache.enable_cli=1",
                "-d", f"opcache.file_cache={file_cache}",
                "-d", "opcache.file_cache_only=0",
                "-d", "opcache.validate_timestamps=0",
                "-d", "opcache.file_update_protection=0",
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
    data = parse_product_output(
        benchmark, completed.stdout, completed.stderr
    )
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
            "-y", str(self.config),
            "-F",
            "-O",
            "-d", f"opcache.enable={int(self.opcache)}",
            "-d", "opcache.validate_timestamps=0",
            "-d", "opcache.file_update_protection=0",
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
                    f"php-fpm exited {self.process.returncode}: "
                    f"{stdout}{stderr}"
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
                "-connect", str(self.socket),
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
        data = parse_product_output(
            benchmark, completed.stdout, completed.stderr
        )
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
    list[dict[str, Any]] | None,
    dict[str, Any] | None,
]:
    def invoke(path: Path = source) -> dict[str, Any]:
        return (
            fpm_pool.request(path, benchmark, target)
            if fpm_pool is not None
            else run_product_cli(
                php, path, benchmark, target, opcache, file_cache
            )
        )

    cold = None
    prime = None
    if opcache:
        cold = []
        for index in range(max(0, samples - 1)):
            if fpm_pool is not None:
                cold_source = source.with_name(
                    f"{source.stem}-cold-{index}{source.suffix}"
                )
                shutil.copyfile(source, cold_source)
                cold.append(invoke(cold_source))
            else:
                shutil.rmtree(file_cache)
                file_cache.mkdir()
                cold.append(invoke())
        if fpm_pool is None:
            shutil.rmtree(file_cache)
            file_cache.mkdir()
        prime = invoke()
        cold.append(prime)
    measured = []
    for _ in range(samples):
        data = invoke()
        if data.get("status") != "returned":
            raise RuntimeError(
                f"{benchmark.name}: product executor returned "
                f"{data.get('status')}"
            )
        measured.append(data)
    return measured, cold, prime


def percentile(values: Iterable[float], fraction: float) -> float:
    ordered = sorted(values)
    if not ordered:
        return 0.0
    index = min(len(ordered) - 1, math.ceil(len(ordered) * fraction) - 1)
    return ordered[index]


def median_metric(
    samples: list[dict[str, Any]], path: tuple[str, ...]
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
                raise RuntimeError(
                    f"{benchmark.name}: {counter}={data[counter]}"
                )
        measured.append(data)
    return measured


def summarize(
    benchmark: Benchmark,
    candidate: list[dict[str, Any]],
    baseline: list[dict[str, Any]] | None,
    reference: list[dict[str, Any]] | None,
    cold: list[dict[str, Any]] | None = None,
    prime: dict[str, Any] | None = None,
) -> dict[str, Any]:
    candidate_results = [sample.get("return_value") for sample in candidate]
    if any(result != candidate_results[0] for result in candidate_results[1:]):
        raise RuntimeError(
            f"{benchmark.name}: candidate return value is nondeterministic"
        )
    if baseline:
        baseline_results = [sample.get("return_value") for sample in baseline]
        if any(result != baseline_results[0] for result in baseline_results[1:]):
            raise RuntimeError(
                f"{benchmark.name}: baseline return value is nondeterministic"
            )
        if baseline_results[0] != candidate_results[0]:
            raise RuntimeError(
                f"{benchmark.name}: candidate returned "
                f"{candidate_results[0]!r}, baseline returned "
                f"{baseline_results[0]!r}"
            )
    if reference:
        reference_results = [
            sample.get("return_value") for sample in reference
        ]
        if any(
            result != reference_results[0] for result in reference_results[1:]
        ):
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
    candidate_execute = median_metric(
        candidate, ("performance", "last_execute_ns")
    )
    if candidate_execute is None:
        candidate_execute = candidate_total / benchmark.repeat
    record: dict[str, Any] = {
        "suite": benchmark.suite,
        "case": benchmark.name,
        "operations": benchmark.operations,
        "candidate_ns_per_operation": (
            candidate_execute / benchmark.operations
        ),
        "candidate_comparable_ns_per_operation": (
            candidate_comparable / benchmark.operations
        ),
        "candidate_bridge_ns": candidate_bridge,
        "candidate_execute_ns": candidate_product_execute,
        "candidate_peak_rss_bytes": median_metric(
            candidate, ("peak_rss_bytes",)
        ),
        "compile_p95_ns": percentile(
            [
                float(sample["performance"]["compile_ns"])
                for sample in candidate
                if isinstance(sample.get("performance"), dict)
            ],
            0.95,
        ),
    }
    if cold is not None:
        cold_wall = median_metric(cold, ("wall_ns",))
        warm_wall = median_metric(candidate, ("wall_ns",))
        record.update(
            {
                "cold_wall_ns": cold_wall,
                "warm_wall_ns": warm_wall,
                "warm_vs_cold": (
                    float(cold_wall) / warm_wall
                    if isinstance(cold_wall, (int, float))
                    and warm_wall is not None and warm_wall > 0
                    else None
                ),
                "cold_opcache_hits": (
                    prime.get("opcache_hits")
                    if prime is not None else None
                ),
                "cold_opcache_misses": (
                    prime.get("opcache_misses")
                    if prime is not None else None
                ),
                "warm_opcache_hits": median_metric(
                    candidate, ("opcache_hits",)
                ),
                "warm_opcache_misses": median_metric(
                    candidate, ("opcache_misses",)
                ),
                "warm_opcache_cached_scripts": median_metric(
                    candidate, ("opcache_cached_scripts",)
                ),
            }
        )
    performance = next(
        (
            sample["performance"]
            for sample in candidate
            if isinstance(sample.get("performance"), dict)
        ),
        None,
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
            "direct_call_frame_bytes",
            "inner_call_runtime_helper_calls",
            "inner_call_heap_allocations",
            "inner_call_catcher_boundaries",
        ):
            record[key] = performance.get(key)
    if baseline:
        baseline_bridge = median_metric(baseline, ("bridge_ns",)) or 0.0
        baseline_execute = baseline_bridge / benchmark.repeat
        record["baseline_ns_per_operation"] = (
            baseline_execute / benchmark.operations
        )
        record["speedup"] = (
            baseline_execute / candidate_comparable
            if candidate_comparable > 0 else 0.0
        )
    if reference:
        reference_execute = median_metric(reference, ("execute_ns",)) or 0.0
        reference_per_operation = (
            reference_execute / benchmark.repeat / benchmark.operations
        )
        record["reference_ns_per_operation"] = reference_per_operation
        record["reference_peak_rss_bytes"] = median_metric(
            reference, ("peak_rss_bytes",)
        )
        record["candidate_vs_reference"] = (
            reference_per_operation
            / record["candidate_ns_per_operation"]
            if record["candidate_ns_per_operation"] > 0 else 0.0
        )
    return record


def geometric_mean(values: Iterable[float]) -> float:
    positive = [value for value in values if value > 0]
    return math.exp(sum(math.log(value) for value in positive) / len(positive))


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
    parser.add_argument(
        "--opcache", choices=("off", "on"), default="off"
    )
    parser.add_argument(
        "--fpm",
        type=Path,
        help="php-fpm binary; inferred beside the candidate CLI binary",
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
        "--target", choices=tuple(TARGET_BY_HOST.values())
    )
    parser.add_argument(
        "--suite", choices=("all", "direct", "hot", "scaling"),
        default="all",
    )
    parser.add_argument(
        "--case",
        action="append",
        dest="cases",
        help="run only the named benchmark case; may be repeated",
    )
    parser.add_argument("--samples", type=int, default=5)
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--enforce", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    host = (platform.system(), platform.machine())
    target = args.target or TARGET_BY_HOST.get(host)
    if target is None:
        raise RuntimeError(f"unsupported benchmark host {host[0]}/{host[1]}")
    if args.samples < 1:
        raise RuntimeError("--samples must be positive")
    for binary in (args.candidate, args.baseline, args.reference):
        if binary is not None and not binary.is_file():
            raise RuntimeError(f"PHP binary does not exist: {binary}")
    if args.mode == "diagnostic" and args.opcache != "off":
        raise RuntimeError(
            "--opcache applies only to product-cli and product-fpm"
        )
    opcache = args.opcache == "on"
    fpm = args.fpm
    cgi_fcgi = args.cgi_fcgi
    if args.mode == "product-fpm":
        if fpm is None:
            fpm = args.candidate.parent.parent / "fpm" / "php-fpm"
        if not fpm.is_file():
            raise RuntimeError(f"php-fpm binary does not exist: {fpm}")
        if cgi_fcgi is None:
            located = shutil.which("cgi-fcgi")
            if located is not None:
                cgi_fcgi = Path(located)
        if cgi_fcgi is None or not cgi_fcgi.is_file():
            raise RuntimeError(
                "product-fpm requires --cgi-fcgi or cgi-fcgi on PATH"
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
                benchmark
                for benchmark in benchmarks
                if benchmark.name in selected
            )
            missing = selected.difference(
                benchmark.name for benchmark in benchmarks
            )
            if missing:
                raise RuntimeError(
                    "unknown benchmark cases: " + ", ".join(sorted(missing))
                )

        file_cache = Path(temp) / "opcache-file-cache"
        file_cache.mkdir()
        product_sources: dict[str, Path] = {}
        if args.mode != "diagnostic":
            for benchmark in benchmarks:
                source = Path(temp) / f"product-{benchmark.name}.php"
                source.write_text(
                    product_source(benchmark), encoding="utf-8"
                )
                product_sources[benchmark.name] = source

        records = []

        def run_benchmarks(fpm_pool: FpmPool | None) -> None:
            for benchmark in benchmarks:
                cold = None
                prime = None
                if args.mode == "diagnostic":
                    candidate = measure(
                        args.candidate, benchmark, target, args.samples,
                        CANDIDATE_RUNNER,
                    )
                else:
                    candidate, cold, prime = measure_product(
                        args.candidate,
                        product_sources[benchmark.name],
                        benchmark,
                        target,
                        args.samples,
                        opcache,
                        file_cache,
                        fpm_pool,
                    )
                baseline_error = None
                try:
                    baseline = (
                        measure(
                            args.baseline, benchmark, target, args.samples,
                            CANDIDATE_RUNNER,
                        )
                        if args.baseline is not None else None
                    )
                except RuntimeError as error:
                    if benchmark.suite != "scaling":
                        raise
                    baseline = None
                    baseline_error = str(error)
                reference = (
                    measure(
                        args.reference, benchmark, target, args.samples,
                        REFERENCE_RUNNER,
                    )
                    if args.reference is not None else None
                )
                record = summarize(
                    benchmark, candidate, baseline, reference, cold, prime
                )
                if baseline_error is not None:
                    record["baseline_error"] = baseline_error
                records.append(record)
                print(json.dumps(record, sort_keys=True), flush=True)

        if args.mode == "product-fpm":
            assert fpm is not None and cgi_fcgi is not None
            fpm_directory = Path(temp) / "fpm"
            fpm_directory.mkdir()
            with FpmPool(
                fpm, cgi_fcgi, fpm_directory, opcache
            ) as fpm_pool:
                run_benchmarks(fpm_pool)
        else:
            run_benchmarks(None)

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
    warm_speedups = [
        float(record["warm_vs_cold"])
        for record in records
        if isinstance(record.get("warm_vs_cold"), (int, float))
    ]
    if warm_speedups:
        summary["warm_geomean_speedup"] = geometric_mean(warm_speedups)
    direct_scalar = next(
        (record for record in records if record["case"] == "scalar_return"),
        None,
    )
    if direct_scalar is not None:
        if "speedup" in direct_scalar:
            summary["direct_scalar_speedup"] = direct_scalar["speedup"]
        if "candidate_vs_reference" in direct_scalar:
            summary["direct_scalar_vs_reference"] = (
                direct_scalar["candidate_vs_reference"]
            )
    independent_1000 = next(
        (
            record
            for record in records
            if record["case"] == "independent_1000"
        ),
        None,
    )
    if independent_1000 is not None:
        summary["independent_1000_compiled_codeunits"] = (
            independent_1000.get("compiled_codeunits")
        )
        if "speedup" in independent_1000:
            summary["independent_1000_speedup"] = (
                independent_1000["speedup"]
            )
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
    print(json.dumps({"summary": summary}, sort_keys=True))

    if not args.enforce:
        return 0
    failures = []
    if args.baseline is None or args.reference is None:
        failures.append("--enforce requires --baseline and --reference")
    if direct_scalar is not None:
        minimum = 0.95 if args.w12_baseline else 3.0
        if summary.get("direct_scalar_speedup", 0) < minimum:
            failures.append(
                "direct scalar call retains less than 95% of W12"
                if args.w12_baseline
                else "direct scalar call speedup is below 3.0x"
            )
        if summary.get("direct_scalar_vs_reference", 0) <= 1.0:
            failures.append("native scalar calls do not beat the reference VM")
    elif args.suite in {"all", "direct"} and not args.cases:
        failures.append("direct scalar benchmark was not executed")
    if hot_speedups:
        minimum = 0.95 if args.w12_baseline else 1.5
        if summary.get("hot_geomean_speedup", 0) < minimum:
            failures.append(
                "hot corpus retains less than 95% of W12"
                if args.w12_baseline
                else "hot corpus geometric mean speedup is below 1.5x"
            )
    elif args.suite in {"all", "hot"} and not args.cases:
        failures.append("hot corpus was not executed")
    if regressions:
        failures.append(
            "representative cases regress by more than 10%: "
            + ", ".join(regressions)
        )
    if opcache and warm_speedups:
        if summary.get("warm_geomean_speedup", 0) <= 1.0:
            failures.append(
                "warm OPcache/FPM requests are not faster than cold requests"
            )
        cold_or_equal = [
            record["case"]
            for record in records
            if isinstance(record.get("warm_vs_cold"), (int, float))
            and float(record["warm_vs_cold"]) <= 1.0
        ]
        if cold_or_equal:
            failures.append(
                "warm requests are not faster for: "
                + ", ".join(cold_or_equal)
            )
    elif opcache and args.mode != "diagnostic":
        failures.append("no cold/warm OPcache samples were recorded")
    if args.mode == "product-fpm" and opcache:
        hit_growth = [
            record
            for record in records
            if isinstance(record.get("cold_opcache_hits"), (int, float))
            and isinstance(record.get("warm_opcache_hits"), (int, float))
            and float(record["warm_opcache_hits"])
                > float(record["cold_opcache_hits"])
        ]
        if not hit_growth:
            failures.append("FPM requests did not produce an OPcache hit")
    if independent_1000 is not None:
        compiled = independent_1000.get("compiled_codeunits")
        scaling_speedup = independent_1000.get("speedup", 0)
        if compiled != 1 and float(scaling_speedup) < 5.0:
            failures.append(
                "1k/1-root compilation is neither lazy nor 5x faster"
            )
    if failures:
        for failure in failures:
            print(f"FAIL: {failure}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
