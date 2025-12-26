# Attribution and Provenance (CC BY 4.0)

This repository is a derivative work based on an upstream snapshot and contains additional
modifications and experiment code.

## Upstream source (snapshot)

Title:
- Connect4 7 x 6 Strong Solution

Author:
- Markus Böck (TU Wien)

Source (snapshot used as the basis of this repository):
- Zenodo record:
  - https://zenodo.org/records/14582823
  - DOI: 10.5281/zenodo.14582823
- Upstream repository (as referenced by the Zenodo record):
  - https://github.com/markus7800/Connect4-Strong-Solver

Upstream file used for reproduction (code + BDD tables):
- `strong_solution_w7_h6_archive.7z`
  - When extracted, it contains the directory `solution_w7_h6/` (among others).
  - For running the experiments in this repository, users are expected to place
    `solution_w7_h6/` at the repository root (i.e., `./solution_w7_h6`).

License:
- Creative Commons Attribution 4.0 International (CC BY 4.0)

## What I changed / added in this repository

Modified file:
- `src/connect4/probe/wdl.c`
  - Modified for the experiment workflow (server/compact probing interface and experiment-specific query handling).

Added files:
- `compile_and_run_experiment_bfs.sh`
  - Builds `experiment-bfs.cpp`, runs it, then runs `alphabeta.py`.
- `experiment-bfs.cpp`
  - BFS-style layer expansion experiment code (memory-bounded by keeping only two consecutive depths).
- `alphabeta.py`
  - Alpha-beta related experiment script.

## License of modifications

My modifications and added files are also provided under CC BY 4.0, consistent with the upstream snapshot.

## How to cite

Please cite:
1) This repository
2) The upstream Zenodo snapshot:
   - Markus Böck, “Connect4 7 x 6 Strong Solution”, Zenodo, DOI: 10.5281/zenodo.14582823
