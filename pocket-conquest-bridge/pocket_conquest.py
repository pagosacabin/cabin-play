"""Pure-stdlib rules engine for Pocket Conquest."""

from __future__ import annotations

import copy
import hashlib
import json
import random
import re
import sys

COLS = "ABCDEF"
ROWS = "12345"
TILE_NAMES = [f"{col}{row}" for row in ROWS for col in COLS]
TILE_SET = frozenset(TILE_NAMES)

SIDES = ("S", "R")
TERRAINS = frozenset(("plain", "hill", "town", "capital"))

BUILD_COST = 5
INCOME_CAPITAL = 5
INCOME_TOWN = 4
INCOME_LAND = 2

HILLS = frozenset(("B1", "C2", "D4", "F1", "A5"))
INITIAL_TOWNS = {
    "D1": ("Northford", 1),
    "B3": ("Crossroads", 2),
    "E5": ("Southmere", 1),
}
INITIAL_CAPITALS = {
    "B5": ("S", 3),
    "E1": ("R", 3),
}

_TILE_RE = re.compile(r"^[A-F][1-5]$")
_POS_INT_RE = re.compile(r"^[1-9][0-9]*$")


def _coords(tile: str) -> tuple[int, int]:
    """Return zero-based (col_idx, row_idx) for a valid tile name."""
    return COLS.index(tile[0]), ROWS.index(tile[1])


def _neighbors(tile: str) -> list[str]:
    """Return orthogonally adjacent tile names in deterministic row-major order."""
    c, r = _coords(tile)
    out: list[str] = []
    for dc, dr in ((0, -1), (-1, 0), (1, 0), (0, 1)):
        nc, nr = c + dc, r + dr
        if 0 <= nc < 6 and 0 <= nr < 5:
            out.append(f"{COLS[nc]}{ROWS[nr]}")
    return out


_NEIGHBORS = {name: tuple(_neighbors(name)) for name in TILE_NAMES}


def _other_side(side: str) -> str:
    """Return the opposing faction identifier."""
    return "R" if side == "S" else "S"


def _is_strict_int(value: object) -> bool:
    """Return True if value is an int and not a bool."""
    return isinstance(value, int) and not isinstance(value, bool)


def _deterministic_rng(payload: object) -> random.Random:
    """Create a deterministic Random instance seeded from a JSON-serialisable object."""
    encoded = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
    seed = int.from_bytes(hashlib.sha256(encoded).digest()[:8], "big")
    return random.Random(seed)


def initial_position() -> dict:
    """Return a fresh initial position dictionary for Pocket Conquest."""
    grid: dict[str, dict] = {}
    for name in TILE_NAMES:
        if name in INITIAL_CAPITALS:
            cap_side, armies = INITIAL_CAPITALS[name]
            grid[name] = {
                "terrain": "capital",
                "owner": cap_side,
                "town": None,
                "capital": cap_side,
                "armies": armies,
                "moved": False,
                "built": False,
                "built_armies": 0,
            }
        elif name in INITIAL_TOWNS:
            town_name, militia = INITIAL_TOWNS[name]
            grid[name] = {
                "terrain": "town",
                "owner": None,
                "town": {"name": town_name, "militia": militia},
                "capital": None,
                "armies": 0,
                "moved": False,
                "built": False,
                "built_armies": 0,
            }
        elif name in HILLS:
            grid[name] = {
                "terrain": "hill",
                "owner": None,
                "town": None,
                "capital": None,
                "armies": 0,
                "moved": False,
                "built": False,
                "built_armies": 0,
            }
        else:
            grid[name] = {
                "terrain": "plain",
                "owner": None,
                "town": None,
                "capital": None,
                "armies": 0,
                "moved": False,
                "built": False,
                "built_armies": 0,
            }
    return {
        "side": "S",
        "gold": {"S": 10, "R": 10},
        "turn": 1,
        "grid": grid,
        "captured_this_turn": [],
    }


def _normalise_tile(name: str, raw_tile: object) -> dict:
    """Validate and normalise a single tile dictionary."""
    if not isinstance(raw_tile, dict):
        raise ValueError(f"Tile {name} must be a dict")
    terrain = raw_tile.get("terrain")
    if terrain not in TERRAINS:
        raise ValueError(f"Tile {name} has invalid terrain: {terrain!r}")

    owner = raw_tile.get("owner")
    if isinstance(owner, str):
        owner = owner.upper()
    if owner is not None and owner not in SIDES:
        raise ValueError(f"Tile {name} has invalid owner: {owner!r}")

    capital = raw_tile.get("capital")
    if isinstance(capital, str):
        capital = capital.upper()
    if terrain == "capital":
        if capital not in SIDES:
            raise ValueError(f"Capital tile {name} must have capital 'S' or 'R'")
    elif capital is not None:
        raise ValueError(f"Non-capital tile {name} cannot have capital set")

    raw_town = raw_tile.get("town")
    town: dict | None = None
    if terrain == "town":
        if not isinstance(raw_town, dict):
            raise ValueError(f"Town tile {name} must have a town dict")
        t_name = raw_town.get("name")
        militia = raw_town.get("militia")
        if not isinstance(t_name, str) or not t_name.strip():
            raise ValueError(f"Town tile {name} must have a non-empty name")
        if not _is_strict_int(militia) or militia < 0:
            raise ValueError(f"Town tile {name} militia must be a non-negative int")
        town = {"name": t_name, "militia": militia}
    elif raw_town is not None:
        raise ValueError(f"Non-town tile {name} cannot have town metadata")

    armies = raw_tile.get("armies", 0)
    if not _is_strict_int(armies) or armies < 0:
        raise ValueError(f"Tile {name} armies must be a non-negative int")
    if armies > 0 and owner is None:
        raise ValueError(f"Tile {name} has armies without an owner")
    if town is not None and town["militia"] > 0:
        if owner is not None or armies != 0:
            raise ValueError(f"Neutral militia town {name} cannot have owner or armies")

    moved = raw_tile.get("moved", False)
    if not isinstance(moved, bool):
        raise ValueError(f"Tile {name} moved flag must be a boolean")
    # `built_armies` is how many of the armies standing here were raised this
    # turn: the RULES make only those immobile, not the whole stack. `built` is
    # kept as the derived boolean so older payloads and the renderer still work.
    built_armies = raw_tile.get("built_armies")
    if built_armies is None:
        built = raw_tile.get("built", False)
        if not isinstance(built, bool):
            raise ValueError(f"Tile {name} built flag must be a boolean")
        built_armies = armies if built else 0
    if not _is_strict_int(built_armies) or built_armies < 0 or built_armies > armies:
        raise ValueError(f"Tile {name} built_armies must be an int between 0 and armies")

    return {
        "terrain": terrain,
        "owner": owner,
        "town": town,
        "capital": capital,
        "armies": armies,
        "moved": moved,
        "built": built_armies > 0,
        "built_armies": built_armies,
    }


def _tile_from_cell(name: str, cell: object) -> dict:
    """Accept the compact diagram notation for a cell and expand it to a tile dict.

    The page's own renderer draws the board as ". H T1 R3* S3*" strings, so a
    payload may hand us those instead of objects: "." empty plain, "H" hill,
    "T<n>" neutral town with militia n, "<S|R><n>" a stack, "<S|R><n>*" that
    side's capital, and an optional trailing "T" (own town) or "~" (moved or
    just built). Already-expanded dicts are passed through untouched.
    """
    if isinstance(cell, dict):
        return cell
    if not isinstance(cell, str):
        raise ValueError(f"Tile {name} must be a dict or a cell string")
    text = cell.strip()
    if text in ("", ".", "-"):
        return {"terrain": "plain"}
    if text.upper() == "H":
        return {"terrain": "hill"}
    town = re.match(r"^[Tt]([0-9]+)$", text)
    if town:
        return {"terrain": "town", "town": {"name": name, "militia": int(town.group(1))}}
    stack = re.match(r"^([SsRr])([0-9]+)([*Tt~]?)$", text)
    if stack:
        side = stack.group(1).upper()
        armies = int(stack.group(2))
        flag = stack.group(3)
        tile = {"terrain": "capital" if flag == "*" else "plain", "owner": side, "armies": armies}
        if flag == "*":
            tile["capital"] = side
        elif flag in ("T", "t"):
            tile["terrain"] = "town"
            tile["town"] = {"name": name, "militia": 0}
        if flag == "~":
            tile["moved"] = True
        return tile
    raise ValueError(f"Tile {name} has an unreadable cell value: {cell!r}")


def normalise_position(raw: object) -> dict:
    """Validate and normalise a raw position payload into canonical wire format."""
    if not isinstance(raw, dict):
        raise ValueError("Position must be a dict")

    side = raw.get("side")
    if isinstance(side, str):
        side = side.upper()
    if side not in SIDES:
        raise ValueError(f"Invalid side: {side!r}")

    raw_gold = raw.get("gold")
    if _is_strict_int(raw_gold):
        # A page that only tracks the player's purse sends one number.
        raw_gold = {side: raw_gold, _other_side(side): 0}
    if not isinstance(raw_gold, dict):
        raise ValueError("Missing or invalid gold")
    gold: dict[str, int] = {}
    for s in SIDES:
        val = raw_gold.get(s)
        if not _is_strict_int(val) or val < 0:
            raise ValueError(f"Gold for {s} must be a non-negative int")
        gold[s] = val

    turn = raw.get("turn", 1)
    if not _is_strict_int(turn) or turn < 1:
        raise ValueError(f"Turn must be an int >= 1, got {turn!r}")

    raw_captured = raw.get("captured_this_turn", [])
    if not isinstance(raw_captured, list):
        raise ValueError("captured_this_turn must be a list")
    captured: list[str] = []
    for item in raw_captured:
        if not isinstance(item, str):
            raise ValueError("captured_this_turn items must be strings")
        norm_item = item.upper()
        if norm_item not in TILE_SET:
            raise ValueError(f"Invalid tile in captured_this_turn: {item!r}")
        if norm_item not in captured:
            captured.append(norm_item)

    raw_grid = raw.get("grid")
    if raw_grid is None and "board" in raw:
        raw_grid = raw["board"]
    if raw_grid is None:
        raise ValueError("Position must include 'grid' or 'board'")

    grid_map: dict[str, object] = {}
    if isinstance(raw_grid, list):
        if len(raw_grid) != 5:
            raise ValueError("List grid must have exactly 5 rows")
        for r_idx, row in enumerate(raw_grid):
            if not isinstance(row, list) or len(row) != 6:
                raise ValueError(f"Grid row {r_idx + 1} must have 6 cells")
            for c_idx, cell in enumerate(row):
                grid_map[f"{COLS[c_idx]}{ROWS[r_idx]}"] = cell
    elif isinstance(raw_grid, dict):
        for k, v in raw_grid.items():
            if not isinstance(k, str):
                raise ValueError("Grid keys must be strings")
            uk = k.upper()
            if uk not in TILE_SET:
                raise ValueError(f"Unknown grid key: {k!r}")
            if uk in grid_map:
                raise ValueError(f"Duplicate grid key: {k!r}")
            grid_map[uk] = v
        if len(grid_map) != 30:
            raise ValueError(f"Grid must contain all 30 tiles, got {len(grid_map)}")
    else:
        raise ValueError("Grid must be a dict of 30 tiles or a 5x6 list")

    grid = {
        name: _normalise_tile(name, _tile_from_cell(name, grid_map[name]))
        for name in TILE_NAMES
    }
    out = {
        "side": side,
        "gold": gold,
        "turn": turn,
        "grid": grid,
        "captured_this_turn": captured,
    }
    if "winner" in raw:
        win = raw["winner"]
        if isinstance(win, str):
            win = win.upper()
        if win is not None and win not in SIDES:
            raise ValueError(f"Invalid winner: {win!r}")
        out["winner"] = win
    return out


def winner(position: dict) -> str | None:
    """Return the winning side ('S' or 'R') if a capital was taken, else None."""
    explicit = position.get("winner")
    if explicit in SIDES:
        return explicit
    for tile in position["grid"].values():
        cap = tile.get("capital")
        if cap in SIDES and tile.get("owner") in SIDES and tile["owner"] != cap:
            return tile["owner"]
    return None


def _is_attack_target(tile: dict, side: str) -> bool:
    """Return True if moving into tile triggers combat for side."""
    if tile["owner"] not in (None, side) and tile["armies"] > 0:
        return True
    if tile["town"] is not None and tile["town"]["militia"] > 0:
        return True
    return False


def _movable_armies(tile: dict) -> int:
    """Armies on this tile that may move: the stack minus any raised this turn."""
    return max(0, tile["armies"] - tile.get("built_armies", 0))


def _parse_move(move: object) -> tuple | None:
    """Parse a move string into a structured tuple or return None if malformed."""
    if not isinstance(move, str) or not move or move != move.strip():
        return None
    parts = move.split(" ")
    if parts == ["END"]:
        return ("END",)
    if parts[0] == "B" and len(parts) == 3:
        tile, count_str = parts[1], parts[2]
        if _TILE_RE.match(tile) and _POS_INT_RE.match(count_str):
            return ("B", tile, int(count_str))
        return None
    if parts[0] == "M" and len(parts) == 4:
        src, dst, count_str = parts[1], parts[2], parts[3]
        if _TILE_RE.match(src) and _TILE_RE.match(dst) and _POS_INT_RE.match(count_str):
            return ("M", src, dst, int(count_str), None, 0)
        return None
    if parts[0] == "M" and len(parts) == 7 and parts[4] == "+":
        src, dst, count_str, sup, sup_str = parts[1], parts[2], parts[3], parts[5], parts[6]
        if (
            _TILE_RE.match(src)
            and _TILE_RE.match(dst)
            and _TILE_RE.match(sup)
            and _POS_INT_RE.match(count_str)
            and _POS_INT_RE.match(sup_str)
        ):
            return ("M", src, dst, int(count_str), sup, int(sup_str))
        return None
    return None


def legal_moves(position: dict) -> list[str]:
    """Return all legal move strings from position in deterministic order."""
    if winner(position) is not None:
        return []
    side = position["side"]
    grid = position["grid"]
    gold = position["gold"][side]
    captured = set(position["captured_this_turn"])
    moves: list[str] = []

    max_build = gold // BUILD_COST
    if max_build >= 1:
        for name in TILE_NAMES:
            tile = grid[name]
            if (
                tile["owner"] == side
                and tile["terrain"] in ("capital", "town")
                and name not in captured
            ):
                for count in range(1, max_build + 1):
                    moves.append(f"B {name} {count}")

    movable = [
        name
        for name in TILE_NAMES
        if grid[name]["owner"] == side
        and _movable_armies(grid[name]) > 0
        and not grid[name]["moved"]
    ]
    movable_set = set(movable)

    for src in movable:
        armies = _movable_armies(grid[src])
        for dst in _NEIGHBORS[src]:
            for count in range(1, armies + 1):
                moves.append(f"M {src} {dst} {count}")
            if _is_attack_target(grid[dst], side):
                for sup in _NEIGHBORS[dst]:
                    if sup == src or sup not in movable_set:
                        continue
                    sup_armies = _movable_armies(grid[sup])
                    for count in range(1, armies + 1):
                        for sup_count in range(1, sup_armies + 1):
                            moves.append(f"M {src} {dst} {count} + {sup} {sup_count}")

    moves.append("END")
    return moves


def is_legal(position: dict, move: object) -> bool:
    """Return True if move is well-formed and legal in position."""
    parsed = _parse_move(move)
    if parsed is None or winner(position) is not None:
        return False
    side = position["side"]
    grid = position["grid"]
    kind = parsed[0]

    if kind == "END":
        return True
    if kind == "B":
        _, tile_name, count = parsed
        tile = grid[tile_name]
        return (
            tile["owner"] == side
            and tile["terrain"] in ("capital", "town")
            and tile_name not in position["captured_this_turn"]
            and count * BUILD_COST <= position["gold"][side]
        )

    _, src_name, dst_name, count, sup_name, sup_count = parsed
    if dst_name not in _NEIGHBORS[src_name]:
        return False
    src = grid[src_name]
    if src["owner"] != side or src["moved"] or count > _movable_armies(src):
        return False
    if sup_name is None:
        return True
    if sup_name == src_name or sup_name not in _NEIGHBORS[dst_name]:
        return False
    if not _is_attack_target(grid[dst_name], side):
        return False
    sup = grid[sup_name]
    return (
        sup["owner"] == side
        and not sup["moved"]
        and sup_count <= _movable_armies(sup)
    )


def resolve_attack(
    attacker_armies: int,
    defender_armies: int,
    defender_on_hill: bool,
    attacker_roll: int,
    defender_roll: int,
    support_armies: int = 0,
) -> dict:
    """Resolve one combat roll deterministically and return totals, winner, and losses."""
    attacker_total = attacker_armies + support_armies + attacker_roll
    defender_total = defender_armies + defender_roll + (1 if defender_on_hill else 0)
    if attacker_total > defender_total:
        margin = attacker_total - defender_total
        return {
            "attacker_total": attacker_total,
            "defender_total": defender_total,
            "winner": "attacker",
            "attacker_losses": 0,
            "defender_losses": 2 if margin >= 3 else 1,
        }
    margin = defender_total - attacker_total
    return {
        "attacker_total": attacker_total,
        "defender_total": defender_total,
        "winner": "defender",
        "attacker_losses": 2 if margin >= 3 else 1,
        "defender_losses": 0,
    }


def _compute_income(position: dict, side: str) -> int:
    """Compute end-of-turn gold income for side."""
    total = 0
    for tile in position["grid"].values():
        if tile["owner"] != side:
            continue
        terrain = tile["terrain"]
        if terrain == "capital":
            total += INCOME_CAPITAL
        elif terrain == "town":
            total += INCOME_TOWN
        else:
            total += INCOME_LAND
    return total


def apply_move(position: dict, move: str, rng: random.Random | None = None) -> dict:
    """Apply a legal move to a copy of position and return the new position dict."""
    pos = copy.deepcopy(normalise_position(position))
    if not is_legal(pos, move):
        raise ValueError(f"Illegal move: {move!r}")

    parsed = _parse_move(move)
    assert parsed is not None
    side = pos["side"]
    grid = pos["grid"]
    kind = parsed[0]

    if kind == "END":
        pos["gold"][side] += _compute_income(pos, side)
        for tile in grid.values():
            tile["moved"] = False
            tile["built"] = False
            tile["built_armies"] = 0
        pos["captured_this_turn"] = []
        win = winner(pos)
        if win is not None:
            pos["winner"] = win
        pos["side"] = _other_side(side)
        if side == "R":
            pos["turn"] += 1
        return pos

    if kind == "B":
        _, tile_name, count = parsed
        pos["gold"][side] -= count * BUILD_COST
        grid[tile_name]["armies"] += count
        grid[tile_name]["built_armies"] += count
        grid[tile_name]["built"] = True
        return pos

    _, src_name, dst_name, count, sup_name, sup_count = parsed
    src = grid[src_name]
    dst = grid[dst_name]

    if _is_attack_target(dst, side):
        roller = rng if rng is not None else _deterministic_rng({"p": pos, "m": move})
        a_roll = roller.randint(1, 6)
        d_roll = roller.randint(1, 6)
        is_militia = dst["town"] is not None and dst["town"]["militia"] > 0
        def_armies = dst["town"]["militia"] if is_militia else dst["armies"]
        result = resolve_attack(
            count,
            def_armies,
            dst["terrain"] == "hill",
            a_roll,
            d_roll,
            support_armies=sup_count,
        )
        if result["winner"] == "attacker":
            remaining_def = max(0, def_armies - result["defender_losses"])
            if is_militia:
                dst["town"]["militia"] = remaining_def
            else:
                dst["armies"] = remaining_def

            if remaining_def == 0:
                src["armies"] -= count
                src["built_armies"] = min(src.get("built_armies", 0), src["armies"])
                if src["armies"] > 0:
                    src["moved"] = True
                else:
                    src["moved"] = False
                    src["built"] = False

                if sup_name is not None:
                    grid[sup_name]["moved"] = True

                if dst["terrain"] in ("town", "capital") and dst["owner"] != side:
                    if dst_name not in pos["captured_this_turn"]:
                        pos["captured_this_turn"].append(dst_name)
                dst["owner"] = side
                dst["armies"] = count
                dst["moved"] = True
                dst["built"] = False
                win = winner(pos)
                if win is not None:
                    pos["winner"] = win
            else:
                src["moved"] = True
                if sup_name is not None:
                    grid[sup_name]["moved"] = True
        else:
            losses = result["attacker_losses"]
            main_loss = min(count, losses)
            src["armies"] -= main_loss
            src["built_armies"] = min(src.get("built_armies", 0), src["armies"])
            if src["armies"] > 0:
                src["moved"] = True
            else:
                src["moved"] = False
                src["built"] = False

            if sup_name is not None:
                sup = grid[sup_name]
                sup_loss = min(sup_count, losses - main_loss)
                sup["armies"] -= sup_loss
                sup["built_armies"] = min(sup.get("built_armies", 0), sup["armies"])
                if sup["armies"] > 0:
                    sup["moved"] = True
                else:
                    sup["moved"] = False
                    sup["built"] = False
        return pos

    # Non-combat move into empty or friendly tile
    src["armies"] -= count
    src["built_armies"] = min(src.get("built_armies", 0), src["armies"])
    if src["armies"] > 0:
        src["moved"] = True
    else:
        src["moved"] = False
        src["built"] = False

    if dst["terrain"] in ("town", "capital") and dst["owner"] != side:
        if dst_name not in pos["captured_this_turn"]:
            pos["captured_this_turn"].append(dst_name)
    dst["owner"] = side
    dst["armies"] += count
    dst["moved"] = True
    win = winner(pos)
    if win is not None:
        pos["winner"] = win
    return pos


def random_legal(position: dict, rng: random.Random | None = None) -> str:
    """Return a legal move from position using rng or deterministic selection."""
    moves = legal_moves(position)
    if not moves:
        raise ValueError("No legal moves available")
    chooser = rng if rng is not None else _deterministic_rng(position)
    return chooser.choice(moves)


def _format_cell(tile: dict) -> str:
    """Format one board cell concisely for text rendering."""
    owner = tile["owner"]
    terrain = tile["terrain"]
    armies = tile["armies"]
    suffix = "~" if (tile["moved"] or tile.get("built_armies", 0) > 0) else ""
    if owner is None:
        if terrain == "hill":
            return "H"
        if terrain == "town":
            return f"T{tile['town']['militia']}"
        return "."
    if terrain == "capital":
        cap = tile["capital"]
        tag = "*" if cap == owner else f"*{cap}"
        return f"{owner}{armies}{tag}{suffix}"
    if terrain == "town":
        return f"{owner}{armies}T{suffix}"
    if terrain == "hill":
        return f"{owner}{armies}H{suffix}"
    return f"{owner}{armies}{suffix}"


def describe(position: dict) -> str:
    """Return a compact text representation of the board for prompts."""
    grid = position["grid"]
    lines = ["   " + " ".join(f"{col:<5}" for col in COLS).rstrip()]
    for row in ROWS:
        cells = [f"{_format_cell(grid[f'{col}{row}']):<5}" for col in COLS]
        lines.append(f" {row} " + " ".join(cells).rstrip())
    return "\n".join(lines)


def selftest() -> int:
    """Run all engine self-tests, print per-scenario results, and return failure count."""
    failures = 0

    def check(name: str, fn) -> None:
        nonlocal failures
        try:
            fn()
            print(f"PASS: {name}")
        except AssertionError as exc:
            failures += 1
            print(f"FAIL: {name} -> {exc}")
        except Exception as exc:
            failures += 1
            print(f"FAIL: {name} -> {type(exc).__name__}: {exc}")

    def test_initial_legal_moves() -> None:
        pos = initial_position()
        moves = legal_moves(pos)
        expected = [
            "B B5 1",
            "B B5 2",
            "M B5 B4 1",
            "M B5 B4 2",
            "M B5 B4 3",
            "M B5 A5 1",
            "M B5 A5 2",
            "M B5 A5 3",
            "M B5 C5 1",
            "M B5 C5 2",
            "M B5 C5 3",
            "END",
        ]
        assert moves == expected, f"Expected {expected}, got {moves}"
        assert "B B5 1" in moves and "M B5 A5 1" in moves and "M B5 C5 1" in moves
        assert not any(m.startswith("M E1 ") for m in moves), "Rival E1 moved on S turn"
        rival_pos = apply_move(pos, "END")
        rival_moves = legal_moves(rival_pos)
        assert "M E1 D1 3" in rival_moves, "Expected rival attack from E1 to D1"

    def test_builds_validation() -> None:
        pos = initial_position()
        assert not is_legal(pos, "B B5 0")
        assert not is_legal(pos, "B B5 3")
        assert not is_legal(pos, "B A5 1")
        assert not is_legal(pos, "B D1 1")
        assert not is_legal(pos, "B Z9 1")
        assert not is_legal(pos, "BAD MOVE")
        pos2 = apply_move(pos, "B B5 2")
        assert pos2["gold"]["S"] == 0
        assert pos2["grid"]["B5"]["armies"] == 5
        assert pos["gold"]["S"] == 10 and pos["grid"]["B5"]["armies"] == 3

        # Build at owned town
        pos_town = copy.deepcopy(pos)
        pos_town["grid"]["D1"]["owner"] = "S"
        pos_town["grid"]["D1"]["town"]["militia"] = 0
        assert is_legal(pos_town, "B D1 2")
        pos_town2 = apply_move(pos_town, "B D1 2")
        assert pos_town2["grid"]["D1"]["armies"] == 2

    def test_moved_and_built_flags() -> None:
        pos = initial_position()
        pos_m = apply_move(pos, "M B5 B4 2")
        assert pos_m["grid"]["B4"]["moved"] is True
        assert pos_m["grid"]["B5"]["moved"] is True
        assert not any(m.startswith("M B4 ") for m in legal_moves(pos_m))
        assert not any(m.startswith("M B5 ") for m in legal_moves(pos_m))

        pos_b = apply_move(pos, "B B5 1")
        assert pos_b["grid"]["B5"]["built"] is True
        assert pos_b["grid"]["B5"]["built_armies"] == 1
        # Only the army raised this turn is immobile: B5 now holds 4 armies, of
        # which 1 was just built, so moves of 1..3 are still legal and 4 is not.
        moves_b = legal_moves(pos_b)
        assert "M B5 B4 3" in moves_b, moves_b
        assert "M B5 B4 4" not in moves_b, moves_b
        # After END the new army is mobile too.
        pos_after = apply_move(pos_b, "END")
        assert pos_after["grid"]["B5"]["built_armies"] == 0
        pos_r = apply_move(pos_after, "END")
        assert "M B5 B4 4" in legal_moves(pos_r), legal_moves(pos_r)

    def test_captured_town_build_restriction() -> None:
        pos = copy.deepcopy(initial_position())
        pos["grid"]["D2"]["owner"] = "S"
        pos["grid"]["D2"]["armies"] = 3
        pos["grid"]["D1"]["town"]["militia"] = 0  # empty neutral town
        pos2 = apply_move(pos, "M D2 D1 2")
        assert pos2["grid"]["D1"]["owner"] == "S"
        assert "D1" in pos2["captured_this_turn"]
        assert not is_legal(pos2, "B D1 1"), "Cannot build at town captured this turn"
        pos_r = apply_move(pos2, "END")
        pos_s_next = apply_move(pos_r, "END")
        assert pos_s_next["captured_this_turn"] == []
        assert is_legal(pos_s_next, "B D1 1"), "Should be able to build at D1 next turn"

    def test_splitting_and_vacating() -> None:
        pos = initial_position()
        moves = legal_moves(pos)
        assert all(f"M B5 B4 {k}" in moves for k in (1, 2, 3))
        assert "M B5 B4 4" not in moves
        assert pos["grid"]["D1"]["owner"] is None
        pos_vac = apply_move(pos, "M B5 B4 3")
        assert pos_vac["grid"]["B5"]["armies"] == 0
        assert pos_vac["grid"]["B5"]["owner"] == "S"
        assert pos_vac["grid"]["B4"]["armies"] == 3
        assert pos_vac["grid"]["B4"]["owner"] == "S"
        pos_r = apply_move(pos_vac, "END")
        pos_s2 = apply_move(pos_r, "END")
        s2_moves = legal_moves(pos_s2)
        b4_moves = [m for m in s2_moves if m.startswith("M B4 ")]
        assert "M B4 B3 3" in b4_moves
        pos_one = copy.deepcopy(pos_s2)
        pos_one["grid"]["B4"]["armies"] = 1
        assert is_legal(pos_one, "M B4 A4 1")
        assert not is_legal(pos_one, "M B4 A4 2")

    def test_resolve_attack() -> None:
        # Margin 1 -> defender loses 1
        r1 = resolve_attack(3, 2, False, 4, 4)
        assert r1 == {
            "attacker_total": 7,
            "defender_total": 6,
            "winner": "attacker",
            "attacker_losses": 0,
            "defender_losses": 1,
        }
        # Margin 3 -> loser loses 2
        r2 = resolve_attack(4, 2, False, 5, 4)
        assert r2["winner"] == "attacker" and r2["defender_losses"] == 2
        # Tie -> defender wins, attacker loses 1
        r3 = resolve_attack(2, 2, False, 3, 3)
        assert r3["winner"] == "defender" and r3["attacker_losses"] == 1 and r3["defender_losses"] == 0
        # Hill +1 -> tie becomes defender win; margin 3 -> attacker loses 2
        r4 = resolve_attack(2, 2, True, 4, 3)
        assert r4["defender_total"] == 6 and r4["winner"] == "defender" and r4["attacker_losses"] == 1
        r5 = resolve_attack(1, 2, True, 1, 2)
        assert r5["attacker_total"] == 2 and r5["defender_total"] == 5 and r5["attacker_losses"] == 2
        # Support adds to attacker total
        r6 = resolve_attack(2, 3, False, 3, 3, support_armies=2)
        assert r6["attacker_total"] == 7 and r6["defender_total"] == 6 and r6["winner"] == "attacker"

    def test_combat_advance_and_pincer() -> None:
        # Defender survives on Crossroads (militia 2) -> attacker stays put
        pos = copy.deepcopy(initial_position())
        pos["grid"]["B4"]["owner"] = "S"
        pos["grid"]["B4"]["armies"] = 3
        # Force attacker 3+2=5 vs militia 2+2=4 -> margin 1 -> 1 militia left
        class FixedRng(random.Random):
            def __init__(self, seq):
                super().__init__(0)
                self._it = iter(seq)
            def randint(self, a, b):
                return next(self._it)

        pos_surv = apply_move(pos, "M B4 B3 3", rng=FixedRng([2, 2]))
        assert pos_surv["grid"]["B3"]["owner"] is None
        assert pos_surv["grid"]["B3"]["town"]["militia"] == 1
        assert pos_surv["grid"]["B4"]["armies"] == 3
        assert pos_surv["grid"]["B4"]["moved"] is True

        # Pincer win: main advances, support stays put and marked moved
        pos_p = copy.deepcopy(initial_position())
        pos_p["grid"]["B4"]["owner"] = "S"
        pos_p["grid"]["B4"]["armies"] = 2
        pos_p["grid"]["C3"]["owner"] = "S"
        pos_p["grid"]["C3"]["armies"] = 2
        pos_win = apply_move(pos_p, "M B4 B3 2 + C3 2", rng=FixedRng([6, 1]))
        assert pos_win["grid"]["B3"]["owner"] == "S"
        assert pos_win["grid"]["B3"]["armies"] == 2
        assert pos_win["grid"]["B4"]["armies"] == 0
        assert pos_win["grid"]["C3"]["armies"] == 2
        assert pos_win["grid"]["C3"]["moved"] is True

    def test_income_and_turn_handoff() -> None:
        pos = copy.deepcopy(initial_position())
        pos["grid"]["A5"]["owner"] = "S"  # hill (+2)
        pos["grid"]["C5"]["owner"] = "S"  # plain (+2)
        pos["grid"]["E5"]["owner"] = "S"  # town (+4)
        pos["grid"]["E5"]["town"]["militia"] = 0
        pos["grid"]["B5"]["moved"] = True
        pos["grid"]["B5"]["built"] = True
        pos["captured_this_turn"] = ["E5"]
        pos_r = apply_move(pos, "END")
        assert pos_r["gold"]["S"] == 10 + 5 + 4 + 2 + 2
        assert pos_r["side"] == "R"
        assert pos_r["turn"] == 1
        assert pos_r["grid"]["B5"]["moved"] is False
        assert pos_r["grid"]["B5"]["built"] is False
        assert pos_r["captured_this_turn"] == []
        pos_s2 = apply_move(pos_r, "END")
        assert pos_s2["side"] == "S"
        assert pos_s2["turn"] == 2
        assert pos_s2["gold"]["R"] == 10 + 5

    def test_win_condition() -> None:
        pos = copy.deepcopy(initial_position())
        pos["grid"]["E2"]["owner"] = "S"
        pos["grid"]["E2"]["armies"] = 6
        pos["grid"]["E1"]["armies"] = 1
        # Let's use explicit roll sequence: attacker 6, defender 1 -> 12 vs 2 -> loss 2 -> 0 def
        class SeqRng(random.Random):
            def __init__(self, seq):
                super().__init__(0)
                self._it = iter(seq)
            def randint(self, a, b):
                return next(self._it)

        pos_won = apply_move(pos, "M E2 E1 6", rng=SeqRng([6, 1]))
        assert winner(pos_won) == "S"
        assert legal_moves(pos_won) == []

    def test_bounds_full_turns_and_normalise() -> None:
        pos = initial_position()
        for _ in range(2):
            # Full round: S turn then R turn
            s_move = next(m for m in legal_moves(pos) if m.startswith("M "))
            pos = apply_move(pos, s_move)
            pos = apply_move(pos, "END")
            r_move = next(m for m in legal_moves(pos) if m.startswith("M "))
            pos = apply_move(pos, r_move)
            pos = apply_move(pos, "END")
        assert pos["turn"] == 3
        for s in SIDES:
            owned = sum(1 for t in pos["grid"].values() if t["owner"] == s)
            assert owned <= 30
        assert len(pos["grid"]) == 30

        # Normalise from 'board' as 5x6 list
        rows = [[copy.deepcopy(pos["grid"][f"{c}{r}"]) for c in COLS] for r in ROWS]
        loose = {"side": "s", "gold": pos["gold"], "board": rows}
        norm = normalise_position(loose)
        assert norm["side"] == "S" and len(norm["grid"]) == 30
        try:
            normalise_position({"side": "X", "gold": {"S": 0, "R": 0}, "grid": {}})
            raise AssertionError("Expected ValueError for bad position")
        except ValueError:
            pass

    check("initial_legal_moves", test_initial_legal_moves)
    check("builds_validation", test_builds_validation)
    check("moved_and_built_flags", test_moved_and_built_flags)
    check("captured_town_build_restriction", test_captured_town_build_restriction)
    check("splitting_and_vacating", test_splitting_and_vacating)
    check("resolve_attack", test_resolve_attack)
    check("combat_advance_and_pincer", test_combat_advance_and_pincer)
    check("income_and_turn_handoff", test_income_and_turn_handoff)
    def test_diagram_cell_input() -> None:
        raw = {
            "side": "S",
            "gold": 10,
            "grid": [
                [".", "H", ".", "T1", "R3*", "H"],
                [".", ".", "H", ".", ".", "."],
                [".", "T2", ".", ".", ".", "."],
                [".", ".", ".", "H", ".", "."],
                ["H", "S3*", ".", ".", "T1", "."],
            ],
        }
        pos = normalise_position(raw)
        assert pos["gold"] == {"S": 10, "R": 0}
        assert pos["grid"]["B5"]["capital"] == "S" and pos["grid"]["B5"]["armies"] == 3
        assert pos["grid"]["B3"]["town"]["militia"] == 2
        assert legal_moves(pos) == legal_moves(initial_position())
        assert is_legal(pos, "M B5 B4 3")

    check("diagram_cell_input", test_diagram_cell_input)
    check("win_condition", test_win_condition)
    check("bounds_full_turns_and_normalise", test_bounds_full_turns_and_normalise)

    try:
        import agent_core  # type: ignore

        print("--- Running agent_core.selftest() ---")
        failures += agent_core.selftest()
    except ImportError:
        pass

    print(f"pocket_conquest selftest total failures: {failures}")
    return failures


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(1 if selftest() > 0 else 0)
