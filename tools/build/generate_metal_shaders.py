#!/usr/bin/env python3
"""Generate the Metal shader headers directly included by the Metal backends.

The manifest is derived from the include graph rather than duplicated here.
Each include of:

  xenia/<area>/shaders/bytecode/metal/<identifier>.h

is matched to exactly one .slang source by applying the same identifier
transformation as xenia-shader-cc (remove .slang, then replace dots with
underscores). Generated files are always placed below the requested generated
root; source-tree bytecode directories are never written.
"""

import argparse
import collections
import dataclasses
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import sys
from typing import Dict, Iterable, List, Optional, Sequence, Tuple


EXPECTED_HEADER_COUNT = 86
EXPECTED_CONSUMER_COUNTS = {
    "src/xenia/gpu/metal/metal_command_processor.cc": 1,
    "src/xenia/gpu/metal/metal_render_target_cache.cc": 21,
    "src/xenia/gpu/metal/metal_texture_cache.cc": 57,
    "src/xenia/ui/metal/metal_immediate_drawer.mm": 2,
    "src/xenia/ui/metal/metal_presenter.mm": 5,
}
CONSUMER_ROOTS = (
    "src/xenia/gpu/metal",
    "src/xenia/ui/metal",
)
CONSUMER_SUFFIXES = {
    ".c",
    ".cc",
    ".cpp",
    ".h",
    ".hpp",
    ".m",
    ".mm",
}
STAGE_NAMES = {
    "cs": "compute",
    "ps": "pixel",
    "vs": "vertex",
}
INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"]'
    r'(?P<include>xenia/(?P<area>gpu|ui)/shaders/bytecode/metal/'
    r'(?P<header>[A-Za-z0-9_.-]+\.h))'
    r'[>"]'
)
HEADER_SYMBOL_TEMPLATE = (
    r"\bconst\s+uint8_t\s+{identifier}_metallib\s*"
    r"\[\s*\]\s*="
)
STATE_SCHEMA = 1


class ManifestError(RuntimeError):
    """The source include graph cannot produce an unambiguous manifest."""


class GenerationError(RuntimeError):
    """A shader could not be generated or validated."""


@dataclasses.dataclass(frozen=True)
class Consumer:
    path: str
    line: int


@dataclasses.dataclass(frozen=True)
class ShaderEntry:
    include: str
    source: Path
    output: Path
    identifier: str
    stage_key: str
    stage_name: str
    consumers: Tuple[Consumer, ...]


def _default_repository() -> Path:
    return Path(__file__).resolve().parents[2]


def _is_relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def _resolve_from_repository(path: Path, repository: Path) -> Path:
    if not path.is_absolute():
        path = repository / path
    return path.resolve()


def _display_path(path: Path, repository: Path) -> str:
    try:
        return path.relative_to(repository).as_posix()
    except ValueError:
        return str(path)


def _shader_identifier(source: Path) -> Tuple[str, str]:
    suffix = ".slang"
    if not source.name.endswith(suffix):
        raise ManifestError(f"not a Slang source: {source}")
    source_stem = source.name[:-len(suffix)]
    if "." not in source_stem:
        raise ManifestError(f"Slang source has no stage suffix: {source}")
    stage_key = source_stem.rsplit(".", 1)[1]
    return source_stem.replace(".", "_"), stage_key


def _source_index(
        repository: Path,
) -> Dict[Tuple[str, str], Tuple[Path, str]]:
    indexed_lists: Dict[Tuple[str, str], List[Tuple[Path, str]]] = (
        collections.defaultdict(list)
    )
    for area in ("gpu", "ui"):
        shader_root = (
            repository / "src" / "xenia" / area / "shaders"
        ).resolve()
        if not shader_root.is_dir():
            raise ManifestError(f"shader source directory is missing: {shader_root}")
        for source in sorted(shader_root.glob("*.slang")):
            resolved_source = source.resolve()
            if not _is_relative_to(resolved_source, shader_root):
                raise ManifestError(
                    f"Slang source escapes its shader directory: {source}"
                )
            identifier, stage_key = _shader_identifier(source)
            indexed_lists[(area, identifier)].append(
                (resolved_source, stage_key)
            )

    duplicates = {
        key: values
        for key, values in indexed_lists.items()
        if len(values) != 1
    }
    if duplicates:
        details = []
        for (area, identifier), values in sorted(duplicates.items()):
            sources = ", ".join(str(value[0]) for value in values)
            details.append(f"{area}/{identifier}: {sources}")
        raise ManifestError(
            "duplicate Slang identifier mappings:\n  "
            + "\n  ".join(details)
        )

    return {
        key: values[0]
        for key, values in indexed_lists.items()
    }


def _scan_consumers(
        repository: Path,
) -> Tuple[Dict[str, List[Consumer]], collections.Counter]:
    includes: Dict[str, List[Consumer]] = collections.defaultdict(list)
    consumer_counts: collections.Counter = collections.Counter()

    for relative_root in CONSUMER_ROOTS:
        consumer_root = (repository / relative_root).resolve()
        if not consumer_root.is_dir():
            raise ManifestError(
                f"Metal consumer directory is missing: {consumer_root}"
            )
        if not _is_relative_to(consumer_root, repository):
            raise ManifestError(
                f"Metal consumer directory escapes the repository: "
                f"{consumer_root}"
            )
        for consumer_path in sorted(
                consumer_root.rglob("*"),
                key=lambda candidate: candidate.as_posix()):
            if (not consumer_path.is_file() or
                    consumer_path.suffix.lower() not in CONSUMER_SUFFIXES):
                continue
            resolved_consumer = consumer_path.resolve()
            if not _is_relative_to(resolved_consumer, consumer_root):
                raise ManifestError(
                    f"Metal consumer escapes its directory: {consumer_path}"
                )
            try:
                lines = consumer_path.read_text(encoding="utf-8").splitlines()
            except UnicodeDecodeError as error:
                raise ManifestError(
                    f"Metal consumer is not UTF-8: {consumer_path}"
                ) from error
            relative_consumer = (
                resolved_consumer.relative_to(repository).as_posix()
            )
            for line_number, line in enumerate(lines, 1):
                match = INCLUDE_RE.match(line)
                if not match:
                    continue
                include = match.group("include")
                include_path = PurePosixPath(include)
                expected_prefix = (
                    "xenia",
                    match.group("area"),
                    "shaders",
                    "bytecode",
                    "metal",
                )
                if (include_path.parts[:-1] != expected_prefix):
                    raise ManifestError(
                        f"invalid Metal bytecode include at "
                        f"{relative_consumer}:{line_number}: {include}"
                    )
                includes[include].append(
                    Consumer(relative_consumer, line_number)
                )
                consumer_counts[relative_consumer] += 1

    return includes, consumer_counts


def discover_manifest(
        repository: Path,
        generated_root: Path,
) -> Tuple[List[ShaderEntry], collections.Counter]:
    repository = repository.resolve()
    generated_root = generated_root.resolve()
    includes, consumer_counts = _scan_consumers(repository)

    include_count = sum(len(consumers) for consumers in includes.values())
    if include_count != EXPECTED_HEADER_COUNT:
        raise ManifestError(
            f"expected {EXPECTED_HEADER_COUNT} direct Metal header includes, "
            f"found {include_count}"
        )
    if len(includes) != EXPECTED_HEADER_COUNT:
        raise ManifestError(
            f"expected {EXPECTED_HEADER_COUNT} unique Metal headers, found "
            f"{len(includes)}"
        )
    if dict(consumer_counts) != EXPECTED_CONSUMER_COUNTS:
        expected = json.dumps(
            EXPECTED_CONSUMER_COUNTS, sort_keys=True, indent=2
        )
        actual = json.dumps(
            dict(sorted(consumer_counts.items())), sort_keys=True, indent=2
        )
        raise ManifestError(
            "Metal shader consumer counts changed.\n"
            f"Expected:\n{expected}\nActual:\n{actual}"
        )

    source_index = _source_index(repository)
    entries: List[ShaderEntry] = []
    seen_outputs: Dict[Path, str] = {}
    seen_identifiers: Dict[str, str] = {}
    seen_sources: Dict[Path, str] = {}

    for include in sorted(includes):
        include_path = PurePosixPath(include)
        area = include_path.parts[1]
        header = include_path.name
        identifier = header[:-len(".h")]
        source_match = source_index.get((area, identifier))
        if source_match is None:
            raise ManifestError(
                f"no .slang source maps to included header {include}"
            )
        source, stage_key = source_match
        stage_name = STAGE_NAMES.get(stage_key)
        if stage_name is None:
            raise ManifestError(
                f"unsupported Metal stage {stage_key!r} for {source}"
            )
        output = (
            generated_root.joinpath(*include_path.parts).resolve()
        )
        if not _is_relative_to(output, generated_root):
            raise ManifestError(
                f"generated output escapes generated root: {output}"
            )
        if output in seen_outputs:
            raise ManifestError(
                f"duplicate generated output {output}: "
                f"{seen_outputs[output]} and {include}"
            )
        if identifier in seen_identifiers:
            raise ManifestError(
                f"duplicate global shader identifier {identifier}: "
                f"{seen_identifiers[identifier]} and {include}"
            )
        if source in seen_sources:
            raise ManifestError(
                f"Slang source maps to multiple outputs: {source}: "
                f"{seen_sources[source]} and {include}"
            )
        seen_outputs[output] = include
        seen_identifiers[identifier] = include
        seen_sources[source] = include
        entries.append(
            ShaderEntry(
                include=include,
                source=source,
                output=output,
                identifier=identifier,
                stage_key=stage_key,
                stage_name=stage_name,
                consumers=tuple(
                    sorted(
                        includes[include],
                        key=lambda consumer: (
                            consumer.path, consumer.line
                        ),
                    )
                ),
            )
        )

    if len(entries) != EXPECTED_HEADER_COUNT:
        raise ManifestError(
            f"expected {EXPECTED_HEADER_COUNT} mapped shaders, found "
            f"{len(entries)}"
        )
    return entries, consumer_counts


def _manifest_document(
        repository: Path,
        generated_root: Path,
        entries: Sequence[ShaderEntry],
        consumer_counts: collections.Counter,
) -> Dict[str, object]:
    return {
        "schema": 1,
        "header_count": len(entries),
        "consumer_counts": dict(sorted(consumer_counts.items())),
        "generated_root": _display_path(generated_root, repository),
        "shaders": [
            {
                "identifier": entry.identifier,
                "stage": entry.stage_key,
                "slang_stage": entry.stage_name,
                "include": entry.include,
                "source": _display_path(entry.source, repository),
                "output": _display_path(entry.output, repository),
                "consumers": [
                    {
                        "path": consumer.path,
                        "line": consumer.line,
                    }
                    for consumer in entry.consumers
                ],
            }
            for entry in entries
        ],
    }


def _canonical_json(value: object) -> str:
    return json.dumps(
        value,
        ensure_ascii=False,
        sort_keys=True,
        separators=(",", ":"),
    )


def _atomic_write_if_changed(path: Path, data: bytes) -> bool:
    if path.is_file() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.tmp.{os.getpid()}")
    try:
        temporary.write_bytes(data)
        os.replace(str(temporary), str(path))
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass
    return True


def _write_manifest_json(path: str, document: Dict[str, object]) -> None:
    text = json.dumps(
        document, ensure_ascii=False, sort_keys=True, indent=2
    ) + "\n"
    if path == "-":
        sys.stdout.write(text)
        return
    _atomic_write_if_changed(Path(path).resolve(), text.encode("utf-8"))


def _sha256_file(
        path: Path,
        cache: Dict[Path, Tuple[int, int, str]],
) -> str:
    resolved = path.resolve()
    stat_result = resolved.stat()
    cached = cache.get(resolved)
    cache_key = (stat_result.st_size, stat_result.st_mtime_ns)
    if cached is not None and cached[:2] == cache_key:
        return cached[2]
    digest = hashlib.sha256()
    with resolved.open("rb") as source_file:
        while True:
            block = source_file.read(1024 * 1024)
            if not block:
                break
            digest.update(block)
    result = digest.hexdigest()
    cache[resolved] = (cache_key[0], cache_key[1], result)
    return result


def _state_path(output: Path) -> Path:
    return output.with_name(output.name + ".state.json")


def _depfile_path(output: Path) -> Path:
    return output.with_name(output.name + ".d")


def _split_make_words(text: str) -> List[str]:
    words: List[str] = []
    current: List[str] = []
    escaped = False
    for character in text:
        if escaped:
            current.append(character)
            escaped = False
        elif character == "\\":
            escaped = True
        elif character.isspace():
            if current:
                words.append("".join(current).replace("$$", "$"))
                current = []
        else:
            current.append(character)
    if escaped:
        current.append("\\")
    if current:
        words.append("".join(current).replace("$$", "$"))
    return words


def _parse_depfile(depfile: Path, repository: Path) -> List[Path]:
    try:
        text = depfile.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as error:
        raise GenerationError(f"unable to read depfile {depfile}: {error}")
    text = text.replace("\\\r\n", "").replace("\\\n", "")

    escaped = False
    separator = -1
    for index, character in enumerate(text):
        if escaped:
            escaped = False
            continue
        if character == "\\":
            escaped = True
            continue
        if character == ":":
            separator = index
            break
    if separator < 0:
        raise GenerationError(f"malformed depfile (missing ':'): {depfile}")

    dependencies: List[Path] = []
    for dependency_text in _split_make_words(text[separator + 1:]):
        dependency = Path(dependency_text)
        if not dependency.is_absolute():
            dependency = repository / dependency
        dependency = dependency.resolve()
        if not dependency.is_file():
            raise GenerationError(
                f"depfile dependency does not exist: {dependency}"
            )
        dependencies.append(dependency)
    if not dependencies:
        raise GenerationError(f"depfile has no dependencies: {depfile}")
    return sorted(set(dependencies), key=lambda path: str(path))


def _escape_make_path(path: Path) -> str:
    value = str(path)
    value = value.replace("$", "$$")
    value = value.replace("\\", "\\\\")
    value = value.replace(" ", "\\ ")
    value = value.replace("#", "\\#")
    return value


def _normalized_depfile(output: Path, dependencies: Sequence[Path]) -> bytes:
    lines = [f"{_escape_make_path(output)}:"]
    for dependency in dependencies:
        lines[-1] += " \\"
        lines.append(f"  {_escape_make_path(dependency)}")
    return ("\n".join(lines) + "\n").encode("utf-8")


def _validate_header(path: Path, identifier: str) -> None:
    if not path.is_file() or path.stat().st_size == 0:
        raise GenerationError(
            f"generated header is missing or empty: {path}"
        )
    try:
        header = path.read_text(encoding="utf-8")
    except UnicodeDecodeError as error:
        raise GenerationError(
            f"generated header is not UTF-8: {path}"
        ) from error
    symbol_re = re.compile(
        HEADER_SYMBOL_TEMPLATE.format(identifier=re.escape(identifier))
    )
    if not symbol_re.search(header):
        raise GenerationError(
            f"generated header {path} does not define exact symbol "
            f"const uint8_t {identifier}_metallib[]"
        )


def _configuration(
        shader_compiler: Path,
        slangc: Path,
        sdk: str,
        metal_std: str,
        minimum_version_flag: str,
        hash_cache: Dict[Path, Tuple[int, int, str]],
) -> Dict[str, object]:
    generator = Path(__file__).resolve()
    return {
        "shader_compiler": str(shader_compiler),
        "shader_compiler_sha256": _sha256_file(
            shader_compiler, hash_cache
        ),
        "slangc": str(slangc),
        "slangc_sha256": _sha256_file(slangc, hash_cache),
        "generator": str(generator),
        "generator_sha256": _sha256_file(generator, hash_cache),
        "sdk": sdk,
        "metal_std": metal_std,
        "minimum_version_flag": minimum_version_flag,
        "mode": "--slang-msl",
    }


def _dependency_records(
        dependencies: Iterable[Path],
        hash_cache: Dict[Path, Tuple[int, int, str]],
) -> List[Dict[str, str]]:
    records = []
    for dependency in sorted(set(dependencies), key=lambda path: str(path)):
        if not dependency.is_file():
            raise GenerationError(
                f"shader dependency is missing: {dependency}"
            )
        records.append({
            "path": str(dependency),
            "sha256": _sha256_file(dependency, hash_cache),
        })
    return records


def _state_fingerprint(payload: Dict[str, object]) -> str:
    return hashlib.sha256(
        _canonical_json(payload).encode("utf-8")
    ).hexdigest()


def _up_to_date(
        entry: ShaderEntry,
        configuration: Dict[str, object],
        repository: Path,
        hash_cache: Dict[Path, Tuple[int, int, str]],
) -> bool:
    depfile = _depfile_path(entry.output)
    state_file = _state_path(entry.output)
    if not entry.output.is_file() or not depfile.is_file() or not state_file.is_file():
        return False
    try:
        _validate_header(entry.output, entry.identifier)
        state = json.loads(state_file.read_text(encoding="utf-8"))
        if not isinstance(state, dict):
            return False
        if state.get("schema") != STATE_SCHEMA:
            return False
        if state.get("identifier") != entry.identifier:
            return False
        if state.get("source") != str(entry.source):
            return False
        if state.get("output") != str(entry.output):
            return False
        if state.get("configuration") != configuration:
            return False

        depfile_dependencies = _parse_depfile(depfile, repository)
        state_dependencies = state.get("dependencies")
        if not isinstance(state_dependencies, list):
            return False
        state_dependency_paths = [
            Path(record["path"]).resolve()
            for record in state_dependencies
            if isinstance(record, dict) and
            isinstance(record.get("path"), str) and
            isinstance(record.get("sha256"), str)
        ]
        if len(state_dependency_paths) != len(state_dependencies):
            return False
        if depfile_dependencies != state_dependency_paths:
            return False
        if entry.source not in state_dependency_paths:
            return False

        current_dependencies = _dependency_records(
            state_dependency_paths, hash_cache
        )
        if current_dependencies != state_dependencies:
            return False
        if state.get("output_sha256") != _sha256_file(
                entry.output, hash_cache):
            return False
        fingerprint_payload = {
            "identifier": entry.identifier,
            "source": str(entry.source),
            "output": str(entry.output),
            "configuration": configuration,
            "dependencies": current_dependencies,
        }
        return state.get("fingerprint") == _state_fingerprint(
            fingerprint_payload
        )
    except (GenerationError, KeyError, OSError, ValueError, json.JSONDecodeError):
        return False


def _generate_entry(
        entry: ShaderEntry,
        shader_compiler: Path,
        slangc: Path,
        sdk: str,
        metal_std: str,
        minimum_version_flag: str,
        configuration: Dict[str, object],
        repository: Path,
        hash_cache: Dict[Path, Tuple[int, int, str]],
) -> None:
    entry.output.parent.mkdir(parents=True, exist_ok=True)
    depfile = _depfile_path(entry.output)
    state_file = _state_path(entry.output)
    temporary_output = entry.output.with_name(
        f".{entry.output.name}.tmp.{os.getpid()}"
    )
    temporary_depfile = depfile.with_name(
        f".{depfile.name}.tmp.{os.getpid()}"
    )
    for temporary in (temporary_output, temporary_depfile):
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass

    command = [
        str(shader_compiler),
        "--slang-msl",
        "--depfile",
        str(temporary_depfile),
        "--metal-sdk",
        sdk,
        "--metal-std",
        metal_std,
        "--metal-min-version-flag",
        minimum_version_flag,
        str(entry.source),
        str(temporary_output),
    ]
    environment = os.environ.copy()
    environment["SLANGC_PATH"] = str(slangc)
    print(f"Generating {entry.identifier}: {shlex.join(command)}")
    try:
        result = subprocess.run(
            command,
            cwd=str(repository),
            env=environment,
            check=False,
        )
        if result.returncode:
            raise GenerationError(
                f"xenia-shader-cc failed for {entry.identifier} with exit "
                f"code {result.returncode}"
            )
        _validate_header(temporary_output, entry.identifier)
        dependencies = _parse_depfile(temporary_depfile, repository)
        if entry.source not in dependencies:
            dependencies.append(entry.source)
            dependencies.sort(key=lambda path: str(path))

        generated_bytes = temporary_output.read_bytes()
        if (entry.output.is_file() and
                entry.output.read_bytes() == generated_bytes):
            temporary_output.unlink()
        else:
            os.replace(str(temporary_output), str(entry.output))
        _validate_header(entry.output, entry.identifier)

        _atomic_write_if_changed(
            depfile, _normalized_depfile(entry.output, dependencies)
        )
        dependency_records = _dependency_records(
            dependencies, hash_cache
        )
        fingerprint_payload = {
            "identifier": entry.identifier,
            "source": str(entry.source),
            "output": str(entry.output),
            "configuration": configuration,
            "dependencies": dependency_records,
        }
        state = {
            "schema": STATE_SCHEMA,
            **fingerprint_payload,
            "output_sha256": _sha256_file(entry.output, hash_cache),
            "fingerprint": _state_fingerprint(fingerprint_payload),
        }
        _atomic_write_if_changed(
            state_file,
            (json.dumps(
                state,
                ensure_ascii=False,
                sort_keys=True,
                indent=2,
            ) + "\n").encode("utf-8"),
        )
    finally:
        for temporary in (temporary_output, temporary_depfile):
            try:
                temporary.unlink()
            except FileNotFoundError:
                pass


def _remove_empty_output_directories(
        entries: Sequence[ShaderEntry],
        generated_root: Path,
) -> None:
    directories = {
        entry.output.parent
        for entry in entries
    }
    for directory in sorted(
            directories, key=lambda path: len(path.parts), reverse=True):
        current = directory
        while current != generated_root and _is_relative_to(
                current, generated_root):
            try:
                current.rmdir()
            except OSError:
                break
            current = current.parent


def clean_outputs(
        entries: Sequence[ShaderEntry],
        generated_root: Path,
) -> int:
    removed = 0
    for entry in entries:
        for path in (
                entry.output,
                _depfile_path(entry.output),
                _state_path(entry.output)):
            if not _is_relative_to(path.resolve(), generated_root):
                raise GenerationError(
                    f"refusing to clean path outside generated root: {path}"
                )
            try:
                path.unlink()
                removed += 1
            except FileNotFoundError:
                pass
    _remove_empty_output_directories(entries, generated_root)
    return removed


def _select_entries(
        entries: Sequence[ShaderEntry],
        requested: Sequence[str],
) -> List[ShaderEntry]:
    if not requested:
        return list(entries)
    duplicates = [
        identifier
        for identifier, count in collections.Counter(requested).items()
        if count > 1
    ]
    if duplicates:
        raise ManifestError(
            "duplicate --shader identifiers: " + ", ".join(sorted(duplicates))
        )
    by_identifier = {
        entry.identifier: entry
        for entry in entries
    }
    unknown = sorted(set(requested) - set(by_identifier))
    if unknown:
        raise ManifestError(
            "unknown --shader identifiers: " + ", ".join(unknown)
        )
    return [
        by_identifier[identifier]
        for identifier in requested
    ]


def _executable(path_text: Optional[str], name: str, repository: Path) -> Path:
    if not path_text:
        raise GenerationError(
            f"{name} is required for generation"
        )
    path = _resolve_from_repository(Path(path_text), repository)
    if not path.is_file():
        raise GenerationError(f"{name} does not exist: {path}")
    if not os.access(str(path), os.X_OK):
        raise GenerationError(f"{name} is not executable: {path}")
    return path


def _argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--repository",
        default=str(_default_repository()),
        help="XeniOS repository root (default: derived from this script)",
    )
    parser.add_argument(
        "--generated-root",
        default="build/generated",
        help="generated output root, relative to the repository by default",
    )
    parser.add_argument(
        "--shader-compiler",
        help="native macOS xenia-shader-cc executable",
    )
    parser.add_argument(
        "--slangc",
        default=os.environ.get("SLANGC_PATH"),
        help="Slang compiler executable (default: SLANGC_PATH)",
    )
    parser.add_argument("--sdk", default="iphoneos")
    parser.add_argument("--metal-std", default="ios-metal2.3")
    parser.add_argument(
        "--deployment-target",
        default=os.environ.get("IPHONEOS_DEPLOYMENT_TARGET", "16.0"),
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="remove only manifest-owned outputs before generation",
    )
    parser.add_argument(
        "--verify-manifest",
        action="store_true",
        help="validate and report the manifest without generating shaders",
    )
    parser.add_argument(
        "--manifest-json",
        metavar="PATH",
        help="write the deterministic manifest as JSON (use - for stdout)",
    )
    parser.add_argument(
        "--shader",
        action="append",
        default=[],
        metavar="IDENTIFIER",
        help="generate one manifest shader; may be repeated",
    )
    return parser


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = _argument_parser().parse_args(argv)
    try:
        repository = Path(args.repository).resolve()
        if not repository.is_dir():
            raise ManifestError(f"repository does not exist: {repository}")
        generated_root = _resolve_from_repository(
            Path(args.generated_root), repository
        )
        entries, consumer_counts = discover_manifest(
            repository, generated_root
        )
        selected_entries = _select_entries(entries, args.shader)
        manifest_document = _manifest_document(
            repository, generated_root, entries, consumer_counts
        )
        if args.manifest_json:
            _write_manifest_json(args.manifest_json, manifest_document)

        print(
            f"Verified Metal shader manifest: {len(entries)} headers "
            f"({sum(consumer_counts.values())} direct includes)"
        )
        for consumer, count in sorted(consumer_counts.items()):
            print(f"  {count:2d}  {consumer}")

        if args.clean:
            removed = clean_outputs(selected_entries, generated_root)
            print(f"Removed {removed} manifest-owned generated files")
        if args.verify_manifest:
            return 0

        if args.sdk != "iphoneos":
            raise GenerationError(
                f"this pipeline targets iphoneos, not {args.sdk!r}"
            )
        if args.metal_std != "ios-metal2.3":
            raise GenerationError(
                f"this pipeline requires ios-metal2.3, not "
                f"{args.metal_std!r}"
            )
        if not re.fullmatch(r"[0-9]+(?:\.[0-9]+){0,2}",
                            args.deployment_target):
            raise GenerationError(
                f"invalid iPhoneOS deployment target: "
                f"{args.deployment_target!r}"
            )

        shader_compiler = _executable(
            args.shader_compiler, "shader compiler", repository
        )
        slangc = _executable(args.slangc, "slangc", repository)
        minimum_version_flag = (
            f"-miphoneos-version-min={args.deployment_target}"
        )
        hash_cache: Dict[Path, Tuple[int, int, str]] = {}
        configuration = _configuration(
            shader_compiler,
            slangc,
            args.sdk,
            args.metal_std,
            minimum_version_flag,
            hash_cache,
        )

        generated = 0
        unchanged = 0
        for entry in selected_entries:
            if _up_to_date(
                    entry, configuration, repository, hash_cache):
                print(f"Up-to-date: {entry.identifier}")
                unchanged += 1
                continue
            _generate_entry(
                entry,
                shader_compiler,
                slangc,
                args.sdk,
                args.metal_std,
                minimum_version_flag,
                configuration,
                repository,
                hash_cache,
            )
            generated += 1

        for entry in selected_entries:
            _validate_header(entry.output, entry.identifier)
        print(
            f"Metal shader generation complete: {generated} generated, "
            f"{unchanged} unchanged, {len(selected_entries)} verified"
        )
        return 0
    except (ManifestError, GenerationError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
