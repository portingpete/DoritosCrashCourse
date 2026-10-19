"""Pin a verified offline shader extension into a native build's input header."""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import sys

try:
    from .compile_title_variants import parse_cache, reflect_ctab, need, validate_source_capture
    from .inspect_pak import _no_links
except ImportError:
    from compile_title_variants import parse_cache, reflect_ctab, need, validate_source_capture
    from inspect_pak import _no_links

IDENTITIES = {
    "source_xex_sha256": "6c4fa5a12ef2ad51db4809d5d654b487300a1ace06c2cdf6cb14258fd273dfc1",
    "package_sha256": "0819b4e9f90695b70e083690e98ac6f76f0df0cf2bbb817f196316c930a54fc3",
    "image_sha256": "e2be3b416bffca18e59c34cbc6d20bc6ff14490a0ec3b1c30e4a0594ce79c8d7",
    "native_cache_sha256": "3ab1cbbb7a7db7a328e0596695556eb2a9c3ebc1e097a65bc5c45afe6c5c0c2d",
    "xenon_cache_sha256": "e0b23484c4b87727b98f6379211bba06e8818c32dcc58cf768b767cb279a5b27",
}

def configure(manifest: Path | None, output: Path) -> None:
    cache_path, digest = "", ""
    source_path, source_digest = "", ""
    if manifest is not None:
        manifest = manifest.absolute()
        _no_links(manifest)
        need(0 < manifest.stat().st_size <= 8*1024*1024, "Invalid shader manifest size")
        data = json.loads(manifest.read_text(encoding="utf-8"))
        for key, expected in IDENTITIES.items():
            need(data.get(key) == expected, "Shader manifest has wrong original identity: " + key)
        need(all(data["trusted_shader_stages"].get(name) is True for name in
                 ("composite/vs", "composite/ps", "multilayer/vs", "multilayer/ps")),
             "Original PC shader baselines were not reproduced")
        cache = Path(data["extension_path"]).absolute()
        need(cache.parent == manifest.parent, "Shader extension must belong to its compiler report")
        _no_links(cache)
        need(0 < cache.stat().st_size <= 4*1024*1024, "Invalid shader extension size")
        code = cache.read_bytes()
        digest = hashlib.sha256(code).hexdigest()
        need(digest == data["extension_sha256"], "Shader extension hash differs from compiler report")
        shaders = parse_cache(code)
        records = {(r["name"].casefold(), tuple(r["key"])): r for r in data["results"]}
        total = 0
        for shader in shaders:
            vertex = shader["name"].casefold().endswith(("/vs", "_vs"))
            for variant in shader["variants"]:
                record = records[(shader["name"].casefold(), variant["key"])]
                need(record["status"] == "verified" and record["exit_code"] == 0 and
                     not record["known_native_key"], "Shader extension contains an unverified or replacement key")
                need(hashlib.sha256(variant["code"]).hexdigest() == record["bytecode_sha256"],
                     "Shader bytecode differs from its verified compiler result")
                reflected = reflect_ctab(variant["code"], vertex, data["parameter_names"])
                need(reflected == sorted(variant["parameters"]) == sorted(map(tuple, record["parameters"])),
                     "Shader extension constant bindings differ from reflection")
                total += 1
        need(total == data["extension_variants"], "Shader extension variant count differs from report")
        cache_path = str(cache)
        count = data.get("source_extension_variants",0)
        need(type(count) is int and 0 <= count <= 1 and count == data.get("source_capture_count",0),
             "Captured original source shaders were not all verified")
        captured_records = {key:r for key,r in records.items() if "source_capture" in r}
        need(len(captured_records) == count, "Original source shader report count differs")
        if count:
            source = Path(data["source_extension_path"]).absolute()
            need(source.parent == manifest.parent, "Original source extension must belong to its compiler report")
            _no_links(source)
            need(0 < source.stat().st_size <= 2*1024*1024, "Invalid original source extension size")
            source_code = source.read_bytes()
            source_digest = hashlib.sha256(source_code).hexdigest()
            need(source_digest == data["source_extension_sha256"], "Original source extension hash differs from report")
            original = parse_cache(source_code,True)
            native_keys = {(s["name"].casefold(),v["key"]) for s in shaders for v in s["variants"]}
            source_total = 0
            for shader in original:
                for variant in shader["variants"]:
                    key = (shader["name"].casefold(),variant["key"])
                    need(key in captured_records and key in native_keys, "Original source shader lacks its paired verified native key")
                    record = captured_records[key]
                    captured = validate_source_capture(record["source_capture"],variant["code"])
                    need(captured["name"].casefold() == key[0] and captured["variant"]["key"] == key[1] and
                         captured["variant"]["parameters"] == variant["parameters"] == list(map(tuple,record["original_parameters"])),
                         "Original source extension changed its captured identity or reflected bindings")
                    original_bindings = {item[0]:item for item in variant["parameters"]}
                    need(all(item[0] in original_bindings and item[2] == original_bindings[item[0]][2]
                             for item in record["parameters"]),
                         "Native source shader constants cannot map exact captured original upload ranges")
                    source_total += 1
            need(source_total == count, "Original source extension variant count differs")
            source_path = str(source)
            print(f"Pinned {count} actual original source shader variants: {source}")
        else:
            need(not data.get("source_extension_path") and not data.get("source_extension_sha256"),
                 "Unexpected original source extension without a measured variant")
        print(f"Pinned {total} offline native shader variants: {cache}")
    _no_links(output.absolute())
    output.parent.mkdir(parents=True, exist_ok=True)
    content = ("#pragma once\nnamespace Doritos::Native::ShaderInputs {\n"
               f"inline constexpr wchar_t path[] = L{json.dumps(cache_path)};\n"
               f"inline constexpr char sha256[] = {json.dumps(digest)};\n"
               f"inline constexpr wchar_t sourcePath[] = L{json.dumps(source_path)};\n"
               f"inline constexpr char sourceSha256[] = {json.dumps(source_digest)};\n}}\n")
    if not output.exists() or output.read_text(encoding="utf-8") != content:
        output.write_text(content, encoding="utf-8")

def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        configure(args.manifest, args.output)
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print("Shader input configuration rejected: " + str(error), file=sys.stderr)
        return 2

if __name__ == "__main__":
    raise SystemExit(main())
