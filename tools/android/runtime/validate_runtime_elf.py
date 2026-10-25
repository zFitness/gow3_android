#!/usr/bin/env python3
"""Inspect and safely rewrite ARM64 runtime ELF interpreters.

The interpreter string lives in a fixed-size PT_INTERP segment.  Replacing it is
safe only when the new string fits that segment; growing it in place can shift
the following ELF data and produce an executable which looks packaged but is
corrupt.  Callers must use an explicit loader/native launcher when it does not
fit.
"""
import argparse
import json
import os
import struct
import sys
from pathlib import Path


ELF_MAGIC = b"\x7fELF"
PT_INTERP = 3
PT_LOAD = 1


class ElfError(ValueError):
    pass


def _header(data):
    if len(data) < 64 or data[:4] != ELF_MAGIC:
        raise ElfError("not an ELF file")
    if data[4] != 2 or data[5] != 1:
        raise ElfError("only little-endian ELF64 is supported")
    fields = struct.unpack_from("<HHIQQQIHHHHHH", data, 16)
    return {
        "machine": fields[1],
        "phoff": fields[4],
        "phentsize": fields[8],
        "phnum": fields[9],
    }


def inspect(path):
    """Return ELF metadata, including PT_INTERP capacity, or None for non-ELF."""
    path = Path(path)
    data = path.read_bytes()
    if not data.startswith(ELF_MAGIC):
        return None
    header = _header(data)
    if header["phentsize"] < 56:
        raise ElfError("program header entry is too small")
    interp = None
    capacity = None
    dynamic = False
    for index in range(header["phnum"]):
        offset = header["phoff"] + index * header["phentsize"]
        if offset + 56 > len(data):
            raise ElfError("program header points outside the file")
        p_type, p_flags, p_offset, _, _, p_filesz, _, _ = struct.unpack_from(
            "<IIQQQQQQ", data, offset)
        if p_type != PT_INTERP:
            if p_type == 2:
                dynamic = True
            continue
        if p_offset + p_filesz > len(data) or p_filesz == 0:
            raise ElfError("invalid PT_INTERP segment")
        raw = data[p_offset:p_offset + p_filesz]
        interp = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
        capacity = p_filesz
    return {
        "path": str(path),
        "machine": header["machine"],
        "dynamic": dynamic,
        "interpreter": interp,
        "interpreter_capacity": capacity,
    }


def rewrite_interpreter(path, interpreter):
    """Rewrite PT_INTERP in place when the new NUL-terminated value fits."""
    info = inspect(path)
    if info is None:
        raise ElfError("not an ELF file")
    if info["interpreter"] is None:
        raise ElfError("ELF has no PT_INTERP segment")
    encoded = interpreter.encode("utf-8") + b"\0"
    capacity = info["interpreter_capacity"]
    if len(encoded) > capacity:
        raise ElfError(
            "PT_INTERP slot too small: %d bytes available, %d required; "
            "use an explicit loader/native launcher" % (capacity, len(encoded)))
    data = Path(path).read_bytes()
    header = _header(data)
    for index in range(header["phnum"]):
        offset = header["phoff"] + index * header["phentsize"]
        p_type, _, p_offset, _, _, p_filesz, _, _ = struct.unpack_from(
            "<IIQQQQQQ", data, offset)
        if p_type == PT_INTERP:
            replacement = encoded.ljust(p_filesz, b"\0")
            with Path(path).open("r+b") as stream:
                stream.seek(p_offset)
                stream.write(replacement)
            return inspect(path)
    raise ElfError("ELF has no PT_INTERP segment")


def validate_tree(root, expected_loader, reject=(), explicit_paths=(), allow_no_interp=()):
    """Validate all ELF files below *root* and return their metadata.

    A non-matching interpreter is reported as requiring the explicit-loader
    strategy.  This intentionally does not accept a longer path merely because
    a rewriting tool offers to rebuild the ELF around it.
    """
    root = Path(root)
    results = []
    explicit_paths = set(explicit_paths)
    allow_no_interp = set(allow_no_interp)
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        try:
            info = inspect(path)
        except ElfError as error:
            raise ElfError("%s: %s" % (path, error)) from error
        if info is None:
            continue
        interpreter = info["interpreter"]
        relative = path.relative_to(root).as_posix()
        # Shared objects normally have no PT_INTERP. A few Debian helper
        # binaries are also valid explicit-loader entrypoints without one;
        # executable entrypoints are enforced by the runtime manifest.
        if interpreter:
            for token in reject:
                if token in interpreter:
                    raise ElfError("%s contains forbidden interpreter %r" % (path, token))
            if interpreter != expected_loader and relative not in explicit_paths:
                raise ElfError(
                    "%s uses %r; expected %r or an explicit-loader/native-launcher entrypoint"
                    % (path, interpreter, expected_loader))
        results.append(info)
    return results


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("paths", nargs="+", type=Path)
    parser.add_argument("--rewrite", metavar="INTERPRETER", help="rewrite PT_INTERP in place if it fits")
    parser.add_argument("--expected-loader", help="require this PT_INTERP value after rewriting")
    parser.add_argument("--reject", action="append", default=["com.gp.android"],
                        help="substring forbidden in interpreter values")
    parser.add_argument("--json", action="store_true", dest="as_json")
    args = parser.parse_args(argv)
    records = []
    try:
        for path in args.paths:
            if args.rewrite:
                rewrite_interpreter(path, args.rewrite)
            info = inspect(path)
            if info is None:
                raise ElfError("%s: not an ELF file" % path)
            if info["interpreter"]:
                for token in args.reject:
                    if token and token in info["interpreter"]:
                        raise ElfError("%s contains forbidden interpreter %r" % (path, token))
                if args.expected_loader and info["interpreter"] != args.expected_loader:
                    raise ElfError("%s: interpreter is %r, expected %r" %
                                   (path, info["interpreter"], args.expected_loader))
            records.append(info)
    except (OSError, ElfError) as error:
        print("validate_runtime_elf: %s" % error, file=sys.stderr)
        return 1
    print(json.dumps(records, indent=2) if args.as_json else
          "validated %d ELF file(s)" % len(records))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
