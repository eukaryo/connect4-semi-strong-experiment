#!/bin/bash
gcc src/connect4/probe/wdl.c -O3 -flto -Wall -O3 -DWIDTH=7 -DHEIGHT=6 -DCOMPRESSED_ENCODING=1 -DALLOW_ROW_ORDER=0 -o wdl.out