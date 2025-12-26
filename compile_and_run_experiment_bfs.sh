#!/bin/bash
set -e
g++ experiment-bfs.cpp -std=c++17 -O2 -march=native -DNDEBUG -fopenmp -pipe -Wall -Wextra -o experiment-bfs.out
./experiment-bfs.out
python3 alphabeta.py
