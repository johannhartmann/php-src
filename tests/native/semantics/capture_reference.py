#!/usr/bin/env python3
"""Capture a provenance-bound W01 semantic reference bundle."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import signal
import subprocess
import sys
import time
from pathlib import Path
from typing import Any, Dict, List, Mapping, Sequence, Tuple

from validate_manifest import ManifestError, load_and_validate


REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
NATIVE_TEST_ROOT = REPOSITORY_ROOT / "tests" / "native"
MANIFEST_PATH = Path(__file__).with_name("manifest.json")
PHPT_TIMEOUT_SECONDS = 180.0
CAPABILITY_SCRIPT = r'''
$required = json_decode($argv[1], true, 512, JSON_THROW_ON_ERROR);
$loaded = [];
foreach ($required as $extension) {
    $loaded[$extension] = extension_loaded($extension);
}
echo json_encode([
    "extensions" => $loaded,
    "php_version_id" => PHP_VERSION_ID,
    "zts" => (bool) PHP_ZTS,
], JSON_UNESCAPED_SLASHES | JSON_THROW_ON_ERROR);
'''


class CaptureError(RuntimeError):
    """A reference capture input, prerequisite, or execution failure."""

    def __init__(self, message: str, exit_code: int = 1):
        super().__init__(message)
        self.exit_code = exit_code


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_record(path: Path, relative_to: Path) -> Dict[str, Any]:
    return {
        "path": path.relative_to(relative_to).as_posix(),
        "sha256": sha256_file(path),
        "size_bytes": path.stat().st_size,
    }


def stable_json_dump(document: Mapping[str, Any], path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as destination:
        json.dump(document, destination, indent=2, sort_keys=True)
        destination.write("\n")
    temporary.replace(path)


def explicit_executable(value: str) -> Path:
    candidate = Path(value)
    if not candidate.is_absolute():
        raise CaptureError("--reference-php must be an explicit absolute path", 2)
    try:
        resolved = candidate.resolve(strict=True)
    except OSError as error:
        raise CaptureError("reference PHP cannot be resolved: {}".format(error), 2)
    if not resolved.is_file() or not os.access(str(resolved), os.X_OK):
        raise CaptureError("reference PHP is not an executable file: {}".format(resolved), 2)
    return resolved


def external_empty_artifact_dir(value: Path) -> Path:
    artifact_dir = value.resolve()
    try:
        artifact_dir.relative_to(REPOSITORY_ROOT)
    except ValueError:
        pass
    else:
        raise CaptureError("--artifact-dir must be outside the repository", 2)
    if artifact_dir.exists():
        if not artifact_dir.is_dir():
            raise CaptureError("--artifact-dir exists and is not a directory", 2)
        if any(artifact_dir.iterdir()):
            raise CaptureError("--artifact-dir must be empty", 2)
    else:
        artifact_dir.mkdir(parents=True)
    return artifact_dir


def deterministic_environment(reference_php: Path, php_config_dir: Path) -> Dict[str, str]:
    environment = {
        "LANG": "C",
        "LC_ALL": "C",
        "NO_INTERACTION": "1",
        "PATH": os.defpath,
        "PHPRC": str(php_config_dir),
        "PHP_INI_SCAN_DIR": "",
        "REPORT_EXIT_STATUS": "1",
        "TEST_PHP_EXECUTABLE": str(reference_php),
        "TZ": "UTC",
    }
    return environment


def required_capabilities(manifest: Mapping[str, Any]) -> Tuple[List[str], bool]:
    extensions = sorted({
        extension
        for fixture in manifest["fixtures"]
        for extension in fixture["required_extensions"]
    })
    require_zts = any("zts" in fixture["required_modes"] for fixture in manifest["fixtures"])
    return extensions, require_zts


def verify_capabilities(
    reference_php: Path,
    manifest: Mapping[str, Any],
    environment: Mapping[str, str],
) -> Dict[str, Any]:
    extensions, require_zts = required_capabilities(manifest)
    completed = subprocess.run(
        [str(reference_php), "-n", "-r", CAPABILITY_SCRIPT, json.dumps(extensions)],
        cwd=str(REPOSITORY_ROOT),
        env=dict(environment),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        timeout=15,
    )
    if completed.returncode != 0:
        raise CaptureError(
            "reference PHP capability query failed: {}".format(
                completed.stderr.decode("utf-8", errors="replace").strip()),
            3,
        )
    try:
        capabilities = json.loads(completed.stdout.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise CaptureError("reference PHP capability query returned invalid JSON: {}".format(error), 3)
    missing = sorted(extension for extension in extensions if not capabilities["extensions"].get(extension, False))
    if missing:
        raise CaptureError("reference PHP lacks required extensions: {}".format(", ".join(missing)), 3)
    if require_zts and capabilities.get("zts") is not True:
        raise CaptureError("reference PHP must be a ZTS build for the declared corpus modes", 3)
    return capabilities


def run_to_files(
    command: Sequence[str],
    cwd: Path,
    environment: Mapping[str, str],
    stdout_path: Path,
    stderr_path: Path,
    timeout: float,
) -> Dict[str, Any]:
    started = time.monotonic_ns()
    timed_out = False
    with stdout_path.open("wb") as stdout_file, stderr_path.open("wb") as stderr_file:
        process = subprocess.Popen(
            list(command),
            cwd=str(cwd),
            env=dict(environment),
            stdout=stdout_file,
            stderr=stderr_file,
            start_new_session=True,
        )
        try:
            returncode = process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
            if os.name == "posix":
                os.killpg(process.pid, signal.SIGKILL)
            else:
                process.kill()
            returncode = process.wait()
    duration_ns = time.monotonic_ns() - started
    if timed_out:
        termination = {"exit_code": None, "signal": None, "timeout": True}
    elif returncode < 0:
        termination = {"exit_code": None, "signal": -returncode, "timeout": False}
    else:
        termination = {"exit_code": returncode, "signal": None, "timeout": False}
    return {"duration_ns": duration_ns, "termination": termination}


def run_checked(command: Sequence[str], environment: Mapping[str, str]) -> None:
    completed = subprocess.run(
        list(command),
        cwd=str(REPOSITORY_ROOT),
        env=dict(environment),
        check=False,
    )
    if completed.returncode != 0:
        raise CaptureError("command failed with exit {}: {}".format(completed.returncode, " ".join(command)))


def phpt_fixture_paths(manifest: Mapping[str, Any]) -> List[str]:
    return sorted({
        fixture["source_path"]
        for fixture in manifest["fixtures"]
        if fixture["kind"] in {"existing_phpt", "new_phpt"}
    })


def semantic_input_paths(manifest: Mapping[str, Any]) -> List[Path]:
    inputs = {
        REPOSITORY_ROOT / fixture["source_path"]
        for fixture in manifest["fixtures"]
    }
    corpus_root = Path(__file__).with_name("corpus")
    inputs.update(path for path in corpus_root.rglob("*") if path.is_file())
    return sorted(inputs, key=lambda path: path.relative_to(REPOSITORY_ROOT).as_posix().encode("utf-8"))


def repository_identity(environment: Mapping[str, str]) -> Dict[str, Any]:
    commit = subprocess.run(
        ["git", "-C", str(REPOSITORY_ROOT), "rev-parse", "HEAD"],
        env=dict(environment),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        text=True,
        timeout=10,
    )
    status = subprocess.run(
        [
            "git", "-C", str(REPOSITORY_ROOT), "status", "--porcelain=v1",
            "--untracked-files=all", "--", "tests/native/semantics",
        ],
        env=dict(environment),
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
        text=True,
        timeout=10,
    )
    if commit.returncode != 0 or status.returncode != 0:
        raise CaptureError("unable to capture semantic corpus Git identity")
    status_lines = status.stdout.splitlines()
    return {
        "commit": commit.stdout.strip(),
        "dirty": bool(status_lines),
        "status": status_lines,
    }


def capture(reference_php_value: str, artifact_dir_value: Path) -> Path:
    reference_php = explicit_executable(reference_php_value)
    artifact_dir = external_empty_artifact_dir(artifact_dir_value)
    php_config_dir = artifact_dir / "php-config"
    php_config_dir.mkdir()
    environment = deterministic_environment(reference_php, php_config_dir)
    try:
        manifest = load_and_validate(MANIFEST_PATH, REPOSITORY_ROOT)
    except (OSError, json.JSONDecodeError, ManifestError) as error:
        raise CaptureError("semantic manifest is invalid: {}".format(error))
    capabilities = verify_capabilities(reference_php, manifest, environment)

    binary_manifest = artifact_dir / "reference-binary-manifest.json"
    run_checked([
        sys.executable,
        str(NATIVE_TEST_ROOT / "capture_manifest.py"),
        "--php",
        str(reference_php),
        "--json-out",
        str(binary_manifest),
    ], environment)

    phpt_stdout = artifact_dir / "phpt.stdout.bin"
    phpt_stderr = artifact_dir / "phpt.stderr.bin"
    phpt_paths = phpt_fixture_paths(manifest)
    phpt_command = [str(reference_php), str(REPOSITORY_ROOT / "run-tests.php"), "-q"] + phpt_paths
    phpt_process = run_to_files(
        phpt_command,
        REPOSITORY_ROOT,
        environment,
        phpt_stdout,
        phpt_stderr,
        PHPT_TIMEOUT_SECONDS,
    )
    phpt_result = {
        "binary_manifest": "reference-binary-manifest.json",
        "capabilities": capabilities,
        "command": phpt_command,
        "deterministic_environment": dict(sorted(environment.items())),
        "duration_ns": phpt_process["duration_ns"],
        "fixture_count": len(phpt_paths),
        "fixtures": phpt_paths,
        "format_version": "1.0.0",
        "result_type": "semantic_phpt_reference",
        "stderr": file_record(phpt_stderr, artifact_dir),
        "stdout": file_record(phpt_stdout, artifact_dir),
        "termination": phpt_process["termination"],
    }
    phpt_result_path = artifact_dir / "phpt-result.json"
    stable_json_dump(phpt_result, phpt_result_path)
    if phpt_process["termination"] != {"exit_code": 0, "signal": None, "timeout": False}:
        raise CaptureError("reference PHPT corpus did not pass; see phpt-result.json")

    self_diff_path = artifact_dir / "reference-self-diff.json"
    run_checked([
        sys.executable,
        str(NATIVE_TEST_ROOT / "diff_runner.py"),
        "--reference",
        str(reference_php),
        "--candidate",
        str(reference_php),
        "--case",
        str(Path(__file__).with_name("corpus") / "differential"),
        "--timeout",
        "5",
        "--json-out",
        str(self_diff_path),
    ], environment)

    payloads = sorted(
        (path for path in artifact_dir.rglob("*") if path.is_file() and path.name != "bundle-index.json"),
        key=lambda path: path.relative_to(artifact_dir).as_posix().encode("utf-8"),
    )
    bundle_index = {
        "artifacts": [file_record(path, artifact_dir) for path in payloads],
        "format_version": "1.0.0",
        "inputs": [file_record(path, REPOSITORY_ROOT) for path in semantic_input_paths(manifest)],
        "manifest": {
            "path": MANIFEST_PATH.relative_to(REPOSITORY_ROOT).as_posix(),
            "sha256": sha256_file(MANIFEST_PATH),
        },
        "purpose": "reference determinism and provenance evidence; not native equivalence",
        "reference_binary": str(reference_php),
        "repository": repository_identity(environment),
        "result_type": "w01_semantic_reference_bundle",
    }
    bundle_index_path = artifact_dir / "bundle-index.json"
    stable_json_dump(bundle_index, bundle_index_path)
    return bundle_index_path


def main(argv: Sequence[str] = ()) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reference-php", required=True, help="explicit absolute reference PHP path")
    parser.add_argument("--artifact-dir", required=True, type=Path)
    args = parser.parse_args(argv or None)
    try:
        bundle_index = capture(args.reference_php, args.artifact_dir)
    except CaptureError as error:
        print("capture_reference.py: {}".format(error), file=sys.stderr)
        return error.exit_code
    print(bundle_index)
    return 0


if __name__ == "__main__":
    sys.exit(main())
