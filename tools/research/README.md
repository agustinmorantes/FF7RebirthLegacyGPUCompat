# Research tools

Small scripts used to analyze the game. None of them are needed to build or install the mod.
Most need `pefile`, `capstone` and `lz4` (`pip install pefile capstone lz4`).

- `strings.py`, `xref.py`, `callers.py`, `disasm.py`: string extraction, references and disassembly in a PE file.
- `pakindex.py`, `pakgrep.py`, `pakfooter.py`: UE4 pak v11 index reader and extraction (stored or zlib entries).
- `shaderarchive.py`, `shaderkinds.py`, `sfi0scan.py`: shader archive parsing, shader kind histogram and dumping, SFI0 feature flags.
- `msanalyze.py`: bulk `dxc -dumpbin` analysis of mesh shaders into a CSV.
- `hashmatch.py`: maps shader hashes in the proxy log to dumped shader files.
- `d3d12caps.py`: D3D12 feature support of the local GPU.
