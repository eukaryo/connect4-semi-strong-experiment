#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <ctype.h>

#include "board.c"
#include "probing.c"

static void rstrip_newline(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == '\n' || s[n-1] == '\r')) {
        s[n-1] = '\0';
        n--;
    }
}

/* ------------------------------
   ADDED: board49 -> (player, mask) decoder for WIDTH=7 HEIGHT=6 style encoding.
   board49 layout: 7 bits per column (col_code), col_code = (2^h - 1) + pattern
     - h: stones in column (0..HEIGHT)
     - pattern: h bits, bottom->top, 0='x', 1='o'
   Side-to-move is determined by parity of total stones (derived_depth).
   ------------------------------ */
static int decode_board49_to_player_mask(uint64_t board49, uint64_t *player_out, uint64_t *mask_out, int *depth_out) {
    // This decoder assumes the 7-bit-per-column encoding, which corresponds to HEIGHT=6.
    if (HEIGHT != 6) {
        // If you ever build other sizes, you can generalize this, but for now keep explicit.
        fprintf(stderr, "ERROR: board49 query requires HEIGHT=6 (compiled HEIGHT=%d)\n", HEIGHT);
        return -1;
    }

    uint64_t xbb = 0;
    uint64_t obb = 0;
    int depth = 0;

    const uint64_t mask7 = (1ULL << 7) - 1ULL; // 0x7f
    const uint64_t max_code = (1ULL << (HEIGHT + 1)) - 2ULL; // for HEIGHT=6 => 126

    for (int col = 0; col < WIDTH; col++) {
        uint64_t col_code = (board49 >> (7 * col)) & mask7;
        if (col_code > max_code) {
            fprintf(stderr, "ERROR: invalid col_code=%" PRIu64 " at col=%d\n", col_code, col);
            return -1;
        }

        // h = floor(log2(col_code+1)) via small loop (<=7 iterations)
        uint64_t t = col_code + 1;
        int h = 0;
        while (t > 1) { t >>= 1; h++; }

        uint64_t base = (h == 0) ? 0ULL : ((1ULL << h) - 1ULL);
        uint64_t pattern = col_code - base; // 0..2^h-1
        depth += h;

        // Set bits for stones (row_from_bottom = 0..h-1)
        for (int row = 0; row < h; row++) {
            uint64_t bit = 1ULL << (col * (HEIGHT + 1) + row);
            if ((pattern >> row) & 1ULL) obb |= bit; // 'o'
            else xbb |= bit;                          // 'x'
        }
    }

    uint64_t mask = xbb | obb;
    uint64_t player = ((depth & 1) == 0) ? xbb : obb; // x to move on even ply, o to move on odd ply

    // Sanity checks (debug only)
    assert((xbb & obb) == 0);
    assert((player & ~mask) == 0);

    *player_out = player;
    *mask_out = mask;
    if (depth_out) *depth_out = depth;
    return 0;
}

static int handle_one_query_from_player_mask(uint64_t player, uint64_t mask, bool compact) {
    const bool terminal = is_terminal(player, mask);

    if (compact) {
        // Output format (one line per query):
        // <terminal:0/1> <v0..v6>   where each vi is -1/0/1 or '.'
        printf("%d", terminal ? 1 : 0);
        if (!terminal) {
            uint8_t move;
            for (move = 0; move < WIDTH; move++) {
                if (is_legal_move(player, mask, move)) {
                    play_column(&player, &mask, move);
                    int8_t res = -probe_board_mmap(player, mask);
                    undo_play_column(&player, &mask, move);
                    printf(" %d", (int)res);
                } else {
                    printf(" .");
                }
            }
        } else {
            for (uint8_t move = 0; move < WIDTH; move++) {
                printf(" .");
            }
        }
        printf("\n");
        fflush(stdout);
        return 0;
    }

    // Verbose output (optional use; kept compatible-ish)
    print_board(player, mask, -1);
    printf("\n");

    int8_t res = probe_board_mmap(player, mask);

    if (res == 1) {
        printf("\n\033[95mOverall evaluation = %d (forced win)\033[0m\n", res);
    } else if (res == 0) {
        printf("\n\033[95mOverall evaluation = %d (forced draw)\033[0m\n", res);
    } else {
        printf("\n\033[95mOverall evaluation = %d (forced loss)\033[0m\n", res);
    }

    if (!terminal) {
        printf("\n");
        printf("\033[95mmove evaluation:\n");
        for (uint8_t move = 0; move < WIDTH; move++) {
            printf("%3d ", move);
        }
        printf("\033[0m\n");

        for (uint8_t move = 0; move < WIDTH; move++) {
            if (is_legal_move(player, mask, move)) {
                play_column(&player, &mask, move);
                res = -probe_board_mmap(player, mask);
                printf("%3d ", res);
                undo_play_column(&player, &mask, move);
            } else {
                printf("  . ");
            }
        }

        printf("\n\n");
        printf(" 1 ... move leads to forced win,\n");
        printf(" 0 ... move leads to forced draw,\n");
        printf("-1 ... move leads to forced loss\n\n");
    } else {
        printf("\033[95m\nGame over.\033[0m\n\n");
    }

    return 0;
}

static int handle_one_query(const char *moveseq, bool compact) {
    uint64_t player = 0;
    uint64_t mask = 0;

    uint8_t move;
    for (size_t i = 0; i < strlen(moveseq); i++) {
        move = (uint8_t)(moveseq[i] - '0');
        assert(0 <= move && move < WIDTH);
        assert(is_legal_move(player, mask, move));
        play_column(&player, &mask, move);
    }

    if (!compact) {
        // Original verbose header (kept compatible)
        printf("input move sequence: %s\n", moveseq);
    }

    return handle_one_query_from_player_mask(player, mask, compact);
}

/* ------------------------------
   ADDED: handler for "B <depth> <board49>" (depth is accepted but not trusted)
   ------------------------------ */
static int handle_one_query_board49(uint64_t board49, int depth_in, bool compact) {
    (void)depth_in; // do not rely on input depth; we derive it from board49

    uint64_t player = 0;
    uint64_t mask = 0;
    int derived_depth = 0;
    if (decode_board49_to_player_mask(board49, &player, &mask, &derived_depth) != 0) {
        // On error, emit a terminal-ish compact line so callers don't hang (optional).
        if (compact) {
            printf("1 . . . . . . .\n");
            fflush(stdout);
        }
        return -1;
    }

    // Optional debug check: input depth should match derived depth
    // (NDEBUGなら消えます)
    assert(depth_in == derived_depth || depth_in == 0 /* allow callers to pass 0 if they want */);

    return handle_one_query_from_player_mask(player, mask, compact);
}

int main(int argc, char const *argv[]) {

    assert(WIDTH <= 10);
    assert(WIDTH * (HEIGHT+1) <= 62);

    bool no_mmap = false;
    bool server = false;
    bool compact = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {

            printf("wdl.out folder moveseq [--compact] [-Xmmap]\n");
            printf("wdl.out folder --server [--compact] [-Xmmap]\n");
            printf("  reads the strong solution for given position (no search for distance to win/loss).\n");
            printf("  folder      ... relative path to folder containing strong solution (bdd_w{width}_h{height}_{ply}_{lost|draw|win}.bin files).\n");
            printf("  moveseq     ... sequence of moves (0 to WIDTH-1) to get position that will be evaluated.\n");
            printf("  -Xmmap      ... disables mmap (strong solution will be read into memory instead. large RAM needed, but no mmap functionality needed). optional.\n");
            printf("  --server    ... read query lines from stdin and answer repeatedly in a single process.\n");
            printf("  --compact   ... print one-line result: <terminal> <v0..v6> (vi in -1/0/1 or '.')\n");
            printf("\n");
            printf("Server mode input:\n");
            printf("  - moveseq line: \"0123...\" (digits)\n");
            printf("  - board49 line: \"B <depth> <board49>\"  (requires HEIGHT=6; depth is accepted, derived depth is used)\n");
            return 0;
        }
        if (strcmp(argv[i], "-Xmmap") == 0) {
            no_mmap = true;
        }
        if (strcmp(argv[i], "--server") == 0) {
            server = true;
        }
        if (strcmp(argv[i], "--compact") == 0) {
            compact = true;
        }
    }

    if (server) {
        if (argc < 2) {
            perror("Wrong number of arguments supplied: see wdl.out -h\n");
            exit(EXIT_FAILURE);
        }
        setvbuf(stdout, NULL, _IOLBF, 0);
    } else {
        setbuf(stdout, NULL);
        if (argc < 3) {
            perror("Wrong number of arguments supplied: see wdl.out -h\n");
            exit(EXIT_FAILURE);
        }
    }

    const char *folder = argv[1];
    chdir(folder);

    if (no_mmap) {
        printf("WARNING: reading *_win.10.bin and *_loss.10.bin of folder %s into memory\n",  folder);
        make_mmaps_read_in_memory(WIDTH, HEIGHT);
    } else {
        make_mmaps(WIDTH, HEIGHT);
    }

    if (!server) {
        const char *moveseq = argv[2];
        // Non-server mode: keep original behavior (moveseq only)
        handle_one_query(moveseq, compact);
    } else {
        // Server loop: one query per line from stdin
        char buf[4096];
        while (fgets(buf, sizeof(buf), stdin) != NULL) {
            rstrip_newline(buf);

            // skip leading whitespace
            char *p = buf;
            while (*p && isspace((unsigned char)*p)) p++;

            // allow empty line = initial position (moveseq)
            if (*p == '\0') {
                handle_one_query(p, compact);
                continue;
            }

            // ADDED: "B <depth> <board49>" support
            if (*p == 'B' || *p == 'b') {
                int depth_in = 0;
                unsigned long long b49 = 0ULL;
                // parse: B <depth> <board49>
                // (depth_in is accepted but not trusted; we derive actual depth from board49)
                if (sscanf(p, "B %d %llu", &depth_in, &b49) == 2) {
                    handle_one_query_board49((uint64_t)b49, depth_in, compact);
                } else {
                    // If parse fails, fall back to moveseq interpretation (for safety)
                    handle_one_query(p, compact);
                }
            } else {
                handle_one_query(p, compact);
            }
        }
    }

    free_mmaps(WIDTH, HEIGHT);
    free(mmaps);
    free(st_sizes);
    free(in_memory);

    return 0;
}
