// single_file_experiment.cpp
// C++17 port of the provided Python experiment code (single translation unit, no external libs).
//
// Patch set included:
//  (1) board49 is carried and updated incrementally (apply_move_board49)
//  (2) reopening_bfs: probes current node, chooses best move (by probing), writes 1-ply children
//  (3) main: BFS by depth using two hash tables H[2], OpenMP parallel over chunks
//  (4) WdlServer reads stdout via fgets(), compact-line parsing is hand-written
//
// NOTE (important):
//  - This program assumes wdl.out supports querying by board49 lines:
//      input line format: "B <depth> <board49>\n"
//    and returns the usual compact response: "<terminal> <v0..v6>\n"
//  - For OpenMP parallelism, we start one wdl.out server per thread (avoid stdout/stderr interleaving).

#include <cassert>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <array>
#include <chrono>
#include <exception>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <cinttypes>  // PRIu64 for fprintf CSV

// POSIX (Linux)
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef _OPENMP
#include <omp.h>
#endif

// -------------------------------
// TT49x8RobinHood (Python port + small extensions)
// -------------------------------
struct TT49x8RobinHood {
    static constexpr int KEY_BITS = 50;
    static constexpr uint64_t KEY_MASK = (uint64_t(1) << KEY_BITS) - 1;
    static constexpr int VAL_SHIFT = KEY_BITS;
    static constexpr uint64_t KEY_MAX = (uint64_t(1) << 49) - 1;

    uint64_t cap = 0;
    std::vector<uint64_t> slots;
    uint64_t size = 0;

    explicit TT49x8RobinHood(uint64_t capacity) {
        if (capacity == 0) throw std::runtime_error("capacity must be positive");
        cap = capacity;
        slots.assign(cap, 0);
        size = 0;
    }

    void clear() {
        // O(cap) clear (as requested)
        for (uint64_t i = 0; i < cap; i++) slots[i] = 0;
        size = 0;
    }

    static uint64_t hash64(uint64_t x) {
        x = x + 0x9E3779B97F4A7C15ULL;
        x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
        x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
        return x ^ (x >> 31);
    }

    inline uint64_t home(uint64_t key_plus) const {
        return hash64(key_plus) % cap;
    }

    inline uint64_t dist(uint64_t idx, uint64_t h) const {
        if (idx >= h) return idx - h;
        return idx + cap - h;
    }

    std::optional<uint16_t> get(uint64_t key) const {
        if (key > KEY_MAX) throw std::runtime_error("key out of 49-bit range");
        uint64_t kp = key + 1;

        uint64_t i = home(kp);
        uint64_t dib = 0;

        while (dib < cap) {
            uint64_t e = slots[i];
            if (e == 0) return std::nullopt;

            uint64_t ekp = e & KEY_MASK;
            if (ekp == kp) {
                uint64_t v = e >> VAL_SHIFT;
                return static_cast<uint16_t>(v);
            }

            uint64_t inc_home = home(ekp);
            uint64_t inc_dib = dist(i, inc_home);
            if (inc_dib < dib) return std::nullopt;

            i++;
            if (i == cap) i = 0;
            dib++;
        }
        return std::nullopt;
    }

    // set_merge: if key exists, merge value14 as:
    // - value2 (bits0-1) must match; keep old
    // - kindmask (bits2-6) is OR-merged
    void set_merge(uint64_t key, uint16_t value14) {
        if (key > KEY_MAX) throw std::runtime_error("key out of 49-bit range");
        if (value14 >= (1u << 14)) throw std::runtime_error("value out of 14-bit range");

        uint64_t kp = key + 1;
        uint64_t entry = kp | (uint64_t(value14) << VAL_SHIFT);

        uint64_t i = home(kp);
        uint64_t dib = 0;

        while (dib < cap) {
            uint64_t e = slots[i];
            if (e == 0) {
                slots[i] = entry;
                size += 1;
                return;
            }

            uint64_t ekp = e & KEY_MASK;
            if (ekp == kp) {
                uint16_t oldv = (uint16_t)(e >> VAL_SHIFT);
                uint16_t old_value2 = oldv & 0x3;
                uint16_t new_value2 = value14 & 0x3;

                // If mismatch happens, it indicates a bug / inconsistent probing.
                // We do not hard-fail here to keep behavior predictable; keep old_value2.
                // (You can change to assert(old_value2 == new_value2) if desired.)
                (void)new_value2;

                uint16_t old_kind = (oldv >> 2) & 0x1F;
                uint16_t new_kind = (value14 >> 2) & 0x1F;
                uint16_t merged_kind = (uint16_t)(old_kind | new_kind);
                uint16_t merged = (uint16_t)(old_value2 | (merged_kind << 2));

                slots[i] = ekp | (uint64_t(merged) << VAL_SHIFT);
                return;
            }

            uint64_t inc_home = home(ekp);
            uint64_t inc_dib = dist(i, inc_home);

            if (inc_dib < dib) {
                // swap
                slots[i] = entry;
                entry = e;
                dib = inc_dib;
            }

            i++;
            if (i == cap) i = 0;
            dib++;
        }

        throw std::runtime_error("TT insertion failed: table seems full");
    }
};

// -------------------------------
// WdlServer (POSIX pipes + fgets)
// -------------------------------
struct WdlServer {
    pid_t pid = -1;
    int fd_in_write = -1;
    int fd_out_read = -1;
    int fd_err_read = -1;

    FILE* out_fp = nullptr;
    FILE* err_fp = nullptr;

    static WdlServer start(const std::string& wdl_bin,
                           const std::string& solution_dir,
                           bool use_in_memory /* -Xmmap */) {
        std::vector<std::string> args;
        args.push_back(wdl_bin);
        args.push_back(solution_dir);
        args.push_back("--server");
        args.push_back("--compact");
        if (use_in_memory) args.push_back("-Xmmap");

        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& s : args) argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);

        int in_pipe[2]{-1,-1};
        int out_pipe[2]{-1,-1};
        int err_pipe[2]{-1,-1};

        if (pipe(in_pipe) != 0) throw std::runtime_error("pipe(in_pipe) failed");
        if (pipe(out_pipe) != 0) throw std::runtime_error("pipe(out_pipe) failed");
        if (pipe(err_pipe) != 0) throw std::runtime_error("pipe(err_pipe) failed");

        pid_t child = fork();
        if (child < 0) throw std::runtime_error("fork failed");

        if (child == 0) {
            dup2(in_pipe[0], STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(err_pipe[1], STDERR_FILENO);

            close(in_pipe[0]); close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            close(err_pipe[0]); close(err_pipe[1]);

            execvp(argv[0], argv.data());
            _exit(127);
        }

        close(in_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[1]);

        WdlServer srv;
        srv.pid = child;
        srv.fd_in_write = in_pipe[1];
        srv.fd_out_read = out_pipe[0];
        srv.fd_err_read = err_pipe[0];

        srv.out_fp = fdopen(srv.fd_out_read, "r");
        srv.err_fp = fdopen(srv.fd_err_read, "r");
        if (!srv.out_fp) throw std::runtime_error("fdopen(stdout) failed");

        return srv;
    }

    static void write_all_fd_buf(int fd, const char* p, size_t n) {
        while (n > 0) {
            ssize_t w = ::write(fd, p, n);
            if (w < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error("write failed");
            }
            p += w;
            n -= size_t(w);
        }
    }

    // Compact-line parser: "<terminal> <v0..v6>" where vi in {-1,0,1,'.'}
    static bool try_parse_compact_line(const char* s, bool& terminal, std::array<int,7>& vals) {
        const unsigned char* p = (const unsigned char*)s;

        while (*p && std::isspace(*p)) p++;
        if (!*p) return false;

        if (*p != '0' && *p != '1') return false;
        terminal = (*p == '1');
        p++;

        for (int i = 0; i < 7; i++) {
            while (*p && std::isspace(*p)) p++;
            if (!*p) return false;

            if (*p == '.') {
                vals[i] = 2; // sentinel for None/illegal
                p++;
            } else {
                int sign = 1;
                if (*p == '-') { sign = -1; p++; }
                if (!std::isdigit(*p)) return false;

                int num = 0;
                while (*p && std::isdigit(*p)) {
                    num = num * 10 + int(*p - '0');
                    p++;
                }
                int v = sign * num;
                if (!(v == -1 || v == 0 || v == 1)) return false;
                vals[i] = v;
            }
        }

        while (*p && std::isspace(*p)) p++;
        if (*p != '\0') return false;
        return true;
    }

    // Query by board49 (requires wdl.out support):
    // send: "B <depth> <board49>\n"
    std::pair<bool, std::array<int,7>> query_board49(uint64_t board49, int depth) {
        char line[128];
        int n = std::snprintf(line, sizeof(line), "B %d %llu\n", depth, (unsigned long long)board49);
        if (n <= 0 || n >= (int)sizeof(line)) throw std::runtime_error("snprintf failed");
        write_all_fd_buf(fd_in_write, line, (size_t)n);

        char buf[4096];
        while (true) {
            if (!out_fp) throw std::runtime_error("out_fp is null");
            if (std::fgets(buf, (int)sizeof(buf), out_fp) == nullptr) {
                std::string err_all;
                if (err_fp) {
                    while (std::fgets(buf, (int)sizeof(buf), err_fp) != nullptr) err_all += buf;
                }
                std::ostringstream oss;
                oss << "wdl server terminated unexpectedly. stderr:\n" << err_all;
                throw std::runtime_error(oss.str());
            }

            size_t L = std::strlen(buf);
            while (L > 0 && (buf[L-1] == '\n' || buf[L-1] == '\r')) {
                buf[L-1] = '\0';
                L--;
            }

            bool terminal = false;
            std::array<int,7> vals{};
            if (try_parse_compact_line(buf, terminal, vals)) {
                return {terminal, vals};
            }
            // ignore other lines (WARNING etc.)
        }
    }

    void close_server() {
        if (fd_in_write != -1) {
            ::close(fd_in_write);
            fd_in_write = -1;
        }
        if (out_fp) { std::fclose(out_fp); out_fp = nullptr; fd_out_read = -1; }
        if (err_fp) { std::fclose(err_fp); err_fp = nullptr; fd_err_read = -1; }

        if (pid != -1) {
            kill(pid, SIGTERM);
            int status = 0;
            waitpid(pid, &status, 0);
            pid = -1;
        }
    }
};

// -------------------------------
// board49 incremental move
// -------------------------------
static constexpr std::array<uint8_t, 7> H_THRESH = {0, 2, 6, 14, 30, 62, 126};
static constexpr std::array<uint8_t, 7> BASE_OF_H = {0, 1, 3, 7, 15, 31, 63};

static inline int h_from_colcode_table(uint64_t col_code) {
    int h = 0;
    while (h < 6 && col_code > H_THRESH[h]) h++;
    return h;
}

static uint64_t apply_move_board49(uint64_t board49, int move_col, int depth) {
    const uint64_t mask7 = (uint64_t(1) << 7) - 1;
    const int col = move_col;

    uint64_t col_code = (board49 >> (7 * col)) & mask7;
    if (col_code > 126) throw std::runtime_error("apply_move_board49: invalid col_code (>126)");

    int h = h_from_colcode_table(col_code);
    if (h >= 6) throw std::runtime_error("apply_move_board49: illegal move (column full)");

    uint64_t base = BASE_OF_H[h];
    uint64_t pattern = col_code - base;

    // x moves at even depth, o moves at odd depth
    bool is_o = ((depth & 1) == 1);
    if (is_o) pattern |= (uint64_t(1) << h);

    int new_h = h + 1;
    uint64_t new_col_code = uint64_t(BASE_OF_H[new_h]) + pattern;
    if (new_col_code > 126) throw std::runtime_error("apply_move_board49: new_col_code out of range");

    uint64_t clear_mask = ~(mask7 << (7 * col));
    board49 = (board49 & clear_mask) | (new_col_code << (7 * col));
    return board49;
}

// -------------------------------
// Node kinds (same as Python)
// -------------------------------
static const int NODEK_P  = 1;
static const int NODEK_Ap = 2;  // A'
static const int NODEK_Pp = 4;  // P'
static const int NODEK_C  = 8;
static const int NODEK_A  = 16;

static const int SOLUTION_MASK = (NODEK_P | NODEK_Ap | NODEK_Pp);

static const std::array<int,7> MOVE_ORDERING = {3,2,4,1,5,0,6};

static int GetChildNodeKind_char(char k, bool is_most_promising) {
    // Encode kinds by char: 'P', 'a'(A'), 'p'(P'), 'C', 'A'
    if (k == 'P') return is_most_promising ? 'P' : 'a';
    if (k == 'a') return is_most_promising ? 'p' : 'C';
    if (k == 'p') return 'a';
    if (k == 'C') return 'A';
    if (k == 'A') return 'C';
    throw std::runtime_error("Invalid node kind in GetChildNodeKind_char");
}

static int nodekinds_of_char(char k) {
    if (k == 'P') return NODEK_P;
    if (k == 'a') return NODEK_Ap;
    if (k == 'p') return NODEK_Pp;
    if (k == 'C') return NODEK_C;
    if (k == 'A') return NODEK_A;
    throw std::runtime_error("Invalid node kind char");
}

// -------------------------------
// Packing helpers
// -------------------------------
// value2 = value + 1 (0..2) where value in {-1,0,1}
// meta14: bits0-1 value2, bits2-6 kindmask(5bit)
static inline uint16_t pack_meta(uint8_t value2, uint8_t kindmask) {
    return (uint16_t)((value2 & 0x3) | ((uint16_t)(kindmask & 0x1F) << 2));
}
static inline uint8_t meta_value2(uint16_t meta) { return (uint8_t)(meta & 0x3); }
static inline uint8_t meta_kindmask(uint16_t meta) { return (uint8_t)((meta >> 2) & 0x1F); }

// child pack (for thread-local vectors):
// bits 0..48  : board49
// bits 49..50 : value2
// bits 51..55 : kindmask
static inline uint64_t pack_child(uint64_t board49, uint8_t value2, uint8_t kindmask) {
    const uint64_t B49_MASK = (uint64_t(1) << 49) - 1;
    return (board49 & B49_MASK) | (uint64_t(value2 & 0x3) << 49) | (uint64_t(kindmask & 0x1F) << 51);
}
static inline uint64_t child_board49(uint64_t x) {
    const uint64_t B49_MASK = (uint64_t(1) << 49) - 1;
    return x & B49_MASK;
}
static inline uint8_t child_value2(uint64_t x) { return (uint8_t)((x >> 49) & 0x3); }
static inline uint8_t child_kindmask(uint64_t x) { return (uint8_t)((x >> 51) & 0x1F); }

// -------------------------------
// Timestamp helper
// -------------------------------
static std::string now_str() {
    auto tp = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(tp);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return std::string(buf);
}

// -------------------------------
// reopening_bfs
// -------------------------------
// Spec per your request:
//  - Probe current position to get wdl_list.
//  - Determine best move (single) using max(wdl) and MOVE_ORDERING.
//  - For each legal move, compute child position, child value, child nodekind, store into out[0..k-1].
//  - Return k = number of legal moves.
static int reopening_bfs(WdlServer& srv,
                         uint64_t board49,
                         int depth,
                         uint8_t kindmask_in,
                         std::array<uint64_t,7>& out_children) {
    // probe
    auto q = srv.query_board49(board49, depth);
    bool is_terminal = q.first;
    const auto& wdl_list = q.second;

    if (is_terminal) return 0;

    // node value and chosen best move
    int value = -999;
    for (int m = 0; m < 7; m++) {
        int v = wdl_list[m];
        if (v != 2) value = std::max(value, v);
    }

    int best_move = -1;
    for (int mv : MOVE_ORDERING) {
        if (wdl_list[mv] != 2 && wdl_list[mv] == value) { best_move = mv; break; }
    }
    if (best_move < 0) throw std::runtime_error("reopening_bfs: failed to find best_move");

    int out_n = 0;

    for (int mv = 0; mv < 7; mv++) {
        if (wdl_list[mv] == 2) continue; // illegal

        bool is_most_promising = (mv == best_move);

        // child kindmask = OR over all parent kinds
        uint8_t child_kmask = 0;
        for (int bit = 0; bit < 5; bit++) {
            int parent_bit = (1 << bit);
            if ((kindmask_in & parent_bit) == 0) continue;

            char pk;
            if (parent_bit == NODEK_P) pk = 'P';
            else if (parent_bit == NODEK_Ap) pk = 'a';
            else if (parent_bit == NODEK_Pp) pk = 'p';
            else if (parent_bit == NODEK_C) pk = 'C';
            else if (parent_bit == NODEK_A) pk = 'A';
            else continue;

            char ck = (char)GetChildNodeKind_char(pk, is_most_promising);
            child_kmask |= (uint8_t)nodekinds_of_char(ck);
        }



        // child board
        uint64_t child_b49 = apply_move_board49(board49, mv, depth);

        // store child's value from child-to-move perspective
        // parent move evaluation is from parent-to-move perspective; negate for child.
        int child_value = -wdl_list[mv];   // in {-1,0,1}
        uint8_t v2 = (uint8_t)(child_value + 1);

        if(kindmask_in == NODEK_C && mv != best_move)continue;

        if((kindmask_in & (NODEK_C | NODEK_A | NODEK_Ap)) == kindmask_in) {
            if(value == 1 && mv != best_move)continue;
        }

        out_children[out_n++] = pack_child(child_b49, v2, child_kmask);
    }

    return out_n;
}

// -------------------------------
// Main (BFS by depth)
// -------------------------------
int main(int argc, char** argv) {
    try {
        (void)argc; (void)argv;

        // H size: "half of previous" (as per your description)
        const uint64_t H_CAP = (((uint64_t(1) << 33) + (uint64_t(1) << 32)) / 2);

        // OpenMP threads
        int num_threads = 1;
#ifdef _OPENMP
        num_threads = omp_get_max_threads();
#endif

        std::cout << now_str() << " : info: starting WdlServers (count = " << num_threads << ")\n";

        // One server per thread (avoid pipe interleaving)
        std::vector<WdlServer> srvs;
        srvs.reserve((size_t)num_threads);
        for (int t = 0; t < num_threads; t++) {
            srvs.push_back(WdlServer::start("./wdl.out", "solution_w7_h6", /*use_in_memory=*/false));
        }

        // init wait: query empty board once per server
        for (int t = 0; t < num_threads; t++) {
            (void)srvs[t].query_board49(0ULL, 0);
        }
        std::cout << now_str() << " : info: WdlServers initialized\n";

        // Two frontier hash tables
        TT49x8RobinHood H0(H_CAP);
        TT49x8RobinHood H1(H_CAP);
        TT49x8RobinHood* H[2] = { &H0, &H1 };

        H[0]->clear();
        H[1]->clear();

        // output CSV (incremental)
        std::FILE* fp = std::fopen("output.csv", "wb");
        if (!fp) {
            std::perror("fopen(output.csv) failed");
            // You can hard-fail if you want:
            // throw std::runtime_error("failed to open output.csv");
        }

        // print header once
        std::cout << "Depth,SolutionArtifactCount,ProofCertificateCount,NodeCount\n";
        if (fp) std::fprintf(fp, "Depth,SolutionArtifactCount,ProofCertificateCount,NodeCount\n");

        // Initialize depth 0 frontier: root board49=0, kind=P, value2 from probing (optional but consistent)
        {
            auto q0 = srvs[0].query_board49(0ULL, 0);
            bool terminal0 = q0.first;
            const auto& w0 = q0.second;
            int root_value = -999;
            if (!terminal0) {
                for (int v : w0) if (v != 2) root_value = std::max(root_value, v);
            } else {
                // terminal empty board shouldn't happen
                root_value = -1;
            }
            uint8_t root_value2 = (uint8_t)(root_value + 1);
            uint8_t root_kindmask = (uint8_t)NODEK_P;
            uint16_t meta = pack_meta(root_value2, root_kindmask);
            H[0]->set_merge(0ULL, meta);
        }

        // Count depth 0
        auto count_depth = [&](TT49x8RobinHood* T, uint64_t& sol, uint64_t& proof) {
            sol = 0; proof = 0;
            for (uint64_t i = 0; i < T->cap; i++) {
                uint64_t e = T->slots[i];
                if (e == 0) continue;
                uint16_t meta = (uint16_t)(e >> TT49x8RobinHood::VAL_SHIFT);
                uint8_t km = meta_kindmask(meta);
                if ((km & SOLUTION_MASK) != 0) sol++;
                else proof++;
            }
        };

        {
            uint64_t sol = 0, proof = 0;
            count_depth(H[0], sol, proof);
            uint64_t node = sol + proof;
            std::cout << 0 << "," << sol << "," << proof << "," << node << "\n";
            if (fp) std::fprintf(fp, "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", 0, sol, proof, node);
        }

        // BFS for depths 1..42
        std::cout << now_str() << " : info: starting BFS\n";

        // OpenMP chunk size for scanning slot array
        const int OMP_CHUNK = (1 << 20);

        for (int depth = 0; depth < 42; depth++) {
            int cur = (depth % 2);
            int nxt = ((depth + 1) % 2);

            H[nxt]->clear();

#pragma omp parallel
            {
#ifdef _OPENMP
                int tid = omp_get_thread_num();
#else
                int tid = 0;
#endif
                WdlServer& srv = srvs[(size_t)tid];

                std::vector<uint64_t> local_children;
                local_children.reserve(1024);

                std::array<uint64_t,7> buf_children{};

#pragma omp for schedule(static, OMP_CHUNK)
                for (long long idx = 0; idx < (long long)H[cur]->slots.size(); idx++) {
                    uint64_t e = H[cur]->slots[(size_t)idx];
                    if (e == 0) continue;

                    uint64_t kp = e & TT49x8RobinHood::KEY_MASK;
                    uint64_t board49 = kp - 1;

                    uint16_t meta = (uint16_t)(e >> TT49x8RobinHood::VAL_SHIFT);
                    uint8_t kindmask_in = meta_kindmask(meta);

                    int nchild = reopening_bfs(srv, board49, depth, kindmask_in, buf_children);
                    for (int j = 0; j < nchild; j++) {
                        local_children.push_back(buf_children[j]);
                    }
                }

#pragma omp critical
                {
                    for (uint64_t pack : local_children) {
                        uint64_t b49 = child_board49(pack);
                        uint8_t v2 = child_value2(pack);
                        uint8_t km = child_kindmask(pack);
                        uint16_t meta = pack_meta(v2, km);
                        H[nxt]->set_merge(b49, meta);
                    }
                }
            } // end parallel

            // Count depth+1
            {
                uint64_t sol = 0, proof = 0;
                count_depth(H[nxt], sol, proof);
                uint64_t node = sol + proof;
                int out_depth = depth + 1;
                std::cout << out_depth << "," << sol << "," << proof << "," << node << "\n";
                if (fp) std::fprintf(fp, "%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", out_depth, sol, proof, node);
            }
        }

        std::cout << now_str() << " : info: BFS finished\n";

        // close servers
        for (auto& s : srvs) s.close_server();

        if (fp) std::fclose(fp);

        std::cout << now_str() << " : info: program finished\n";
        return 0;

    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 1;
    }
}
