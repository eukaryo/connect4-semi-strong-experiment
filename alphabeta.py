from __future__ import annotations

import datetime
import subprocess
from array import array
from dataclasses import dataclass


class TT49x8RobinHood:
    """
    Robin Hood hashing, open addressing, no deletes.
    key: 49-bit unsigned (0 .. 2^49-1)
    value: 14-bit unsigned (0 .. 16383)
    capacity: any positive integer (not necessarily power-of-two)

    Slot layout (uint64):
      bits 0..49  : key_plus = key + 1   (0 means empty)
      bits 50..63 : value (14-bit)

    NOTE:
      We do not store DIB in-slot; we recompute incumbent DIB by re-hashing its key.
      This keeps packing compact and capacity arbitrary, at the cost of extra hashing.
    """

    KEY_BITS = 50
    KEY_MASK = (1 << KEY_BITS) - 1
    VAL_SHIFT = KEY_BITS
    KEY_MAX = (1 << 49) - 1

    def __init__(self, capacity: int):
        if capacity <= 0:
            raise ValueError("capacity must be positive")
        self.cap = int(capacity)
        self.slots = array("Q", [0]) * self.cap
        self.size = 0

    @staticmethod
    def _hash64(x: int) -> int:
        """
        SplitMix64-ish mixing for 64-bit keys.
        Deterministic, fast, good distribution.
        """
        x &= (1 << 64) - 1
        x = (x + 0x9E3779B97F4A7C15) & ((1 << 64) - 1)
        x = ((x ^ (x >> 30)) * 0xBF58476D1CE4E5B9) & ((1 << 64) - 1)
        x = ((x ^ (x >> 27)) * 0x94D049BB133111EB) & ((1 << 64) - 1)
        return x ^ (x >> 31)

    def _home(self, key_plus: int) -> int:
        # capacity is arbitrary => use modulo
        return self._hash64(key_plus) % self.cap

    def _dist(self, idx: int, home: int) -> int:
        # modular distance from home to idx in [0, cap)
        if idx >= home:
            return idx - home
        return idx + self.cap - home

    def get(self, key: int) -> int | None:
        if key < 0 or key > self.KEY_MAX:
            raise ValueError("key out of 49-bit range")
        kp = key + 1
        cap = self.cap
        slots = self.slots

        i = self._home(kp)
        dib = 0

        while dib < cap:
            e = slots[i]
            if e == 0:
                return None

            ekp = e & self.KEY_MASK
            if ekp == kp:
                return e >> self.VAL_SHIFT

            # Robin Hood early-exit condition: if incumbent's DIB < our DIB, key is not present
            inc_home = self._home(ekp)
            inc_dib = self._dist(i, inc_home)
            if inc_dib < dib:
                return None

            i += 1
            if i == cap:
                i = 0
            dib += 1

        return None

    def set(self, key: int, value: int) -> None:
        if key < 0 or key > self.KEY_MAX:
            raise ValueError("key out of 49-bit range")
        if value < 0 or value >= (1 << 14):
            print(f"value={value} {bin(value)}")
            raise ValueError("value out of 14-bit range")

        kp = key + 1
        entry = kp | (int(value) << self.VAL_SHIFT)

        cap = self.cap
        slots = self.slots

        i = self._home(kp)
        dib = 0

        while dib < cap:
            e = slots[i]
            if e == 0:
                slots[i] = entry
                self.size += 1
                return

            ekp = e & self.KEY_MASK
            if ekp == kp:
                # update in place
                slots[i] = entry
                return

            # Compute incumbent DIB
            inc_home = self._home(ekp)
            inc_dib = self._dist(i, inc_home)

            # Robin Hood rule: steal from the rich (smaller DIB)
            if inc_dib < dib:
                # swap: place our entry here, keep probing with displaced entry
                slots[i], entry = entry, e
                dib = inc_dib  # displaced entry's DIB at this index
                # after moving one step forward, its DIB increases by 1
                # (we'll do dib += 1 at the end of the loop)

            i += 1
            if i == cap:
                i = 0
            dib += 1


@dataclass
class WdlServer:
    proc: subprocess.Popen[str]

    @classmethod
    def start(
        cls,
        *,
        wdl_bin: str = "./wdl.out",
        solution_dir: str = "solution_w7_h6",
        use_in_memory: bool = False,  # -Xmmap
    ) -> "WdlServer":
        cmd = [wdl_bin, solution_dir, "--server", "--compact"]
        if use_in_memory:
            cmd.append("-Xmmap")

        proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            bufsize=1,
        )
        assert proc.stdin is not None
        assert proc.stdout is not None
        return cls(proc=proc)

    @staticmethod
    def _parse_compact_line(line: str) -> tuple[bool, list[int | None]]:
        toks = line.strip().split()
        if len(toks) != 8:
            raise ValueError(f"bad token count: {len(toks)} in {line!r}")
        if toks[0] not in ("0", "1"):
            raise ValueError(f"bad terminal flag: {toks[0]!r} in {line!r}")
        terminal = toks[0] == "1"
        vals: list[int | None] = []
        for t in toks[1:]:
            vals.append(None if t == "." else int(t))
        return terminal, vals

    def query(self, move_seq: str) -> tuple[bool, list[int | None]]:
        assert self.proc.stdin is not None
        assert self.proc.stdout is not None

        self.proc.stdin.write(move_seq + "\n")
        self.proc.stdin.flush()

        while True:
            line = self.proc.stdout.readline()
            if line == "":
                stderr = ""
                if self.proc.stderr is not None:
                    stderr = self.proc.stderr.read()
                raise RuntimeError(
                    f"wdl server terminated unexpectedly. stderr:\n{stderr}"
                )

            s = line.strip()
            if not s:
                continue

            toks = s.split()
            if len(toks) == 8 and toks[0] in ("0", "1"):
                return WdlServer._parse_compact_line(s)

    def close(self) -> None:
        if self.proc.stdin is not None:
            try:
                self.proc.stdin.close()
            except Exception:
                pass

        if self.proc.poll() is None:
            self.proc.terminate()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)


def moveseq_to_board_42(moveseq: str, width: int = 7, height: int = 6) -> str:
    """
    Convert a connect4 move sequence (string of digits) into a 42-char board string.

    - Board indexing: rows top->bottom, cols left->right (same orientation as typical printouts).
    - Cell chars: '.' empty, 'x' first player, 'o' second player.
    - Raises ValueError on illegal moves (column full) or invalid digits.

    Example:
        moveseq_to_board_42("3333332")
    """
    # board[row][col], row=0 top ... height-1 bottom
    board: list[list[str]] = [["."] * width for _ in range(height)]
    heights = [0] * width  # number of stones already in each column (from bottom)

    for ply, ch in enumerate(moveseq):
        if not ("0" <= ch <= "9"):
            raise ValueError(f"Invalid move character {ch!r} at ply={ply}")
        col = ord(ch) - ord("0")
        if not (0 <= col < width):
            raise ValueError(f"Move out of range: {col} at ply={ply} (width={width})")

        if heights[col] >= height:
            raise ValueError(f"Illegal move: column {col} is full at ply={ply}")

        row_from_bottom = heights[col]  # 0 means bottom row
        row = height - 1 - row_from_bottom
        stone = "x" if (ply % 2 == 0) else "o"

        board[row][col] = stone
        heights[col] += 1

    return "".join("".join(board[r]) for r in range(height))


def board49_to_board42(board49: int, width: int = 7, height: int = 6) -> str:
    """
    Decode the collision-free 49-bit board encoding (7 bits per column, base-128 digits)
    back into a 42-char board string (rows top->bottom, cols left->right).

    Column encoding (must match the encoder):
      col_code = (2^h - 1) + pattern
        - h: number of stones in the column (0..height)
        - pattern: h-bit integer, bottom->top
            bit i = 0 => 'x', 1 => 'o'  (i=0 is bottom stone)

    Raises ValueError if a column code is invalid for the given height.
    """
    if board49 < 0:
        raise ValueError("board49 must be non-negative")

    # initialize empty board
    board = [["."] * width for _ in range(height)]

    mask7 = (1 << 7) - 1  # 0x7f

    for col in range(width):
        col_code = (board49 >> (7 * col)) & mask7

        # For height=6, valid codes are 0..126.
        # More generally: max_code = 2^(height+1) - 2
        max_code = (1 << (height + 1)) - 2
        if col_code > max_code:
            raise ValueError(
                f"Invalid col_code={col_code} at col={col} for height={height}"
            )

        # Decode h using: h = floor(log2(col_code + 1))
        h = (col_code + 1).bit_length() - 1
        if h > height:
            raise ValueError(f"Invalid height h={h} at col={col} (height={height})")

        base = (1 << h) - 1
        pattern = col_code - base  # 0 .. (2^h - 1)

        # Fill stones from bottom to top
        for row_from_bottom in range(h):
            row = height - 1 - row_from_bottom
            is_o = (pattern >> row_from_bottom) & 1
            board[row][col] = "o" if is_o else "x"

    return "".join("".join(board[r]) for r in range(height))


def moveseq_to_board49(moveseq: str, width: int = 7, height: int = 6) -> int:
    """
    Convert a connect4 move sequence (string of digits) into a collision-free 49-bit board encoding.

    Encoding (same as board42_to_board49):
      - Per column, keep height h (0..6) and an h-bit pattern from bottom to top:
          bit i = 0 for 'x' (first), 1 for 'o' (second)
      - Column code: col_code = (2^h - 1) + pattern   (0..126 fits in 7 bits)
      - Board code:  board49 = Σ col_code[col] << (7*col)

    Raises ValueError on invalid digits, out-of-range moves, or a full column.
    """
    if width != 7 or height != 6:
        # This encoding works for any fixed height<=63, but keep checks explicit.
        if not (1 <= width and 1 <= height <= 63):
            raise ValueError(
                f"Unsupported width/height: width={width}, height={height}"
            )

    heights = [0] * width  # stones in each column
    patterns = [0] * width  # bit i (from bottom) is 1 iff the stone at i is 'o'

    for ply, ch in enumerate(moveseq):
        if not ("0" <= ch <= "9"):
            raise ValueError(f"Invalid move character {ch!r} at ply={ply}")
        col = ord(ch) - ord("0")
        if not (0 <= col < width):
            raise ValueError(f"Move out of range: {col} at ply={ply} (width={width})")

        h = heights[col]
        if h >= height:
            raise ValueError(f"Illegal move: column {col} is full at ply={ply}")

        # 'x' on even ply, 'o' on odd ply
        if (ply & 1) == 1:
            patterns[col] |= 1 << h

        heights[col] = h + 1

    board49 = 0
    for col in range(width):
        h = heights[col]
        pattern = patterns[col] & ((1 << h) - 1)  # mask for safety
        col_code = ((1 << h) - 1) + pattern  # 0..126 fits in 7 bits
        board49 |= col_code << (7 * col)

    return board49


srv = WdlServer.start(
    wdl_bin="./wdl.out", solution_dir="solution_w7_h6", use_in_memory=False
)
srv.query("")

TRANSPOSITION_TABLE = TT49x8RobinHood(capacity=(1 << 33) + (1 << 32))

MOVE_ORDERING = [3, 2, 4, 1, 5, 0, 6]


def search(moveseq: str, alpha: int, beta: int) -> int:
    board = moveseq_to_board49(moveseq)

    lb = -1
    ub = 1

    tt_value = TRANSPOSITION_TABLE.get(board)
    if tt_value is not None:
        lb = (tt_value % 16) - 1
        ub = (tt_value // 16) - 1
        if lb >= beta:
            return lb
        if ub <= alpha:
            return ub
        alpha = max(alpha, lb)
        beta = min(beta, ub)

    is_terminal, wdl_list = srv.query(moveseq)

    if is_terminal:
        if len(moveseq) == 42:
            _, _wdl_list = srv.query(moveseq[:-1])
            if max(x for x in _wdl_list if x is not None) == 1:
                value = -1
            else:
                value = 0
        else:
            value = -1
        TRANSPOSITION_TABLE.set(board, (value + 1) + ((value + 1) * 16))  # store exact
        return value

    value = max(x for x in wdl_list if x is not None)

    if beta <= value:  # beta-cutoff will occur
        for move in MOVE_ORDERING:
            if wdl_list[move] == value:
                next_moveseq = moveseq + str(move)
                child_value = -search(next_moveseq, -beta, -alpha)
                assert child_value == value
                assert child_value >= beta
                alpha = value
                beta = value
                break
        TRANSPOSITION_TABLE.set(board, (value + 1) + ((ub + 1) * 16))
    else:
        for move in MOVE_ORDERING:
            if wdl_list[move] == value:
                first_move = move
                next_moveseq = moveseq + str(move)
                child_value = -search(next_moveseq, -alpha - 1, -alpha)
                assert child_value == value
                assert child_value < beta
                alpha = max(alpha, value)
                break
        for move in MOVE_ORDERING:
            if move == first_move:
                continue
            if wdl_list[move] is not None:
                next_moveseq = moveseq + str(move)
                child_value = -search(next_moveseq, -beta, -alpha)
                assert child_value <= alpha

        TRANSPOSITION_TABLE.set(board, (value + 1) + ((value + 1) * 16))
    return value


if __name__ == "__main__":
    print(
        f"{datetime.datetime.now().strftime(r'%Y/%m/%d %H:%M:%S')} : info: starting search"
    )
    value = search("", -1, 1)
    print(
        f"{datetime.datetime.now().strftime(r'%Y/%m/%d %H:%M:%S')} : info: search completed. value = {value} , Transposition table size = {TRANSPOSITION_TABLE.size}"
    )

    srv.close()

    node_count = [0] * 43

    for i in range(len(TRANSPOSITION_TABLE.slots)):
        e = TRANSPOSITION_TABLE.slots[i]
        if e != 0:
            kp = e & TT49x8RobinHood.KEY_MASK
            board = kp - 1
            board_str = board49_to_board42(board)

            val = e >> TT49x8RobinHood.VAL_SHIFT
            node_kind = val >> 8

            node_count[sum(1 for c in board_str if c != ".")] += 1

    print("Depth,NodeCount")
    for depth in range(43):
        print(f"{depth},{node_count[depth]}")

    print(
        f"{datetime.datetime.now().strftime(r'%Y/%m/%d %H:%M:%S')} : info: program finished"
    )

"""
taki@taki-MS-7C37:~/strong_solution_w7_h6_archive$ pypy3 alphabeta.py
2025/12/17 20:07:56 : info: starting search
2025/12/17 21:15:02 : info: search completed. value = 1 , Transposition table size = 96450672
Depth 0: 1 nodes
Depth 1: 1 nodes
Depth 2: 7 nodes
Depth 3: 7 nodes
Depth 4: 47 nodes
Depth 5: 47 nodes
Depth 6: 260 nodes
Depth 7: 257 nodes
Depth 8: 1082 nodes
Depth 9: 1056 nodes
Depth 10: 3747 nodes
Depth 11: 3506 nodes
Depth 12: 12442 nodes
Depth 13: 10924 nodes
Depth 14: 39826 nodes
Depth 15: 34201 nodes
Depth 16: 118507 nodes
Depth 17: 100607 nodes
Depth 18: 313016 nodes
Depth 19: 264026 nodes
Depth 20: 736986 nodes
Depth 21: 621892 nodes
Depth 22: 1547226 nodes
Depth 23: 1309926 nodes
Depth 24: 2857888 nodes
Depth 25: 2440699 nodes
Depth 26: 4598788 nodes
Depth 27: 3969020 nodes
Depth 28: 6473206 nodes
Depth 29: 5651727 nodes
Depth 30: 7927180 nodes
Depth 31: 7034569 nodes
Depth 32: 8489314 nodes
Depth 33: 7646337 nodes
Depth 34: 7706523 nodes
Depth 35: 7000444 nodes
Depth 36: 5843979 nodes
Depth 37: 5397649 nodes
Depth 38: 3259327 nodes
Depth 39: 3080195 nodes
Depth 40: 983599 nodes
Depth 41: 970631 nodes
Depth 42: 0 nodes
2025/12/17 21:18:48 : info: program finished
"""
