"""Page <-> engine game-state adapter for the /games API.

The Pocket Conquest page sends its own state shape (`statePayload()` in
index.html): a `board` of 30 tiles keyed by grid name, each
{terrain, name, owner, armies, ready}, plus `gold: {nord, ember}` and a numeric
`turn` (= ceil(ply/2)). It polls `GET /games/{id}` and adopts the reply when
`state.turn === "nord"`, copying per tile `owner`, `armies`, `immobile` and
`acquiredPly`, plus `gold`, `ply` and `winner` (`adoptState()`).

The rules authority is `pocket_conquest.py`, which speaks sides S/R on a `grid`
with explicit town/capital metadata and `moved`/`built_armies` flags. This
module is the only place the two shapes meet, and it keeps the page's own
vocabulary at the wire:

  * terrain  page "plains"/"hills"  <->  engine "plain"/"hill"
  * owner    page "nord"/"ember"    <->  engine "S"/"R"
             page "neutral"         <->  engine owner None + town militia
  * mobility page `ready` (movable armies)  <->  engine `moved`/`built_armies`
             immobile = armies - ready, so a partially spent stack stays
             partially movable instead of being flattened to "ready or not".
  * ply      page ply = 2*turn-1 on nord's turn, 2*turn on ember's.
  * capital  the engine's `capital` field is the tile's ORIGINAL side (it is
             how `winner()` detects a captured capital), so it is taken from
             the board definition, never from the current owner.

Nothing here trusts the caller: every field is type- and range-checked, unknown
tiles are rejected, and the whole board must be present. A rejected state is a
400, which the page already treats as "use the computer".
"""

from __future__ import annotations

import secrets
import threading
import time

import pocket_conquest as pc

MAX_GAMES = 500
GAME_TTL_SECONDS = 6 * 60 * 60
MAX_ID_LEN = 32
MAX_NAME_LEN = 40
MAX_ARMIES = 999
MAX_GOLD = 9999
MAX_TURN = 9999

PAGE_TERRAIN_TO_ENGINE = {
    "plains": "plain",
    "hills": "hill",
    "plain": "plain",
    "hill": "hill",
    "town": "town",
    "capital": "capital",
}
ENGINE_TERRAIN_TO_PAGE = {"plain": "plains", "hill": "hills", "town": "town", "capital": "capital"}
PAGE_OWNER_TO_ENGINE = {"nord": "S", "ember": "R"}
ENGINE_OWNER_TO_PAGE = {"S": "nord", "R": "ember"}
# The board definition's capitals (index.html CAPITALS): the engine needs the
# tile's original side even after it has been captured.
CAPITAL_SIDE = {"B5": "S", "E1": "R"}
DEFAULT_TOWN_NAMES = {"D1": "Northford", "B3": "Crossroads", "E5": "Southmere"}


class GameStateError(ValueError):
    """A payload that cannot be turned into a legal position."""


def _int(value, field, minimum, maximum):
    if isinstance(value, bool) or not isinstance(value, int):
        raise GameStateError("%s must be an integer" % field)
    if value < minimum or value > maximum:
        raise GameStateError("%s must be between %d and %d" % (field, minimum, maximum))
    return value


def _owner(value, field):
    if value is None:
        return None
    if not isinstance(value, str):
        raise GameStateError("%s must be a string or null" % field)
    key = value.strip().lower()
    if key == "neutral":
        return "neutral"
    if key not in PAGE_OWNER_TO_ENGINE:
        raise GameStateError("%s is not nord, ember or neutral" % field)
    return key


def engine_position_from_page(raw, side):
    """Convert one page state payload into a canonical engine position.

    `side` is the side that is about to act ("S" for nord, "R" for ember).
    Raises GameStateError on anything malformed or inconsistent.
    """
    if side not in ("S", "R"):
        raise GameStateError("side must be S or R")
    if not isinstance(raw, dict):
        raise GameStateError("state must be an object")

    board = raw.get("board")
    if not isinstance(board, dict) or len(board) != len(pc.TILE_NAMES):
        raise GameStateError("state.board must hold all 30 tiles")
    for key in board:
        if not isinstance(key, str) or key.upper() not in pc.TILE_SET:
            raise GameStateError("state.board has an unknown tile %r" % (key[:8],))

    raw_gold = raw.get("gold")
    if not isinstance(raw_gold, dict):
        raise GameStateError("state.gold must be an object")
    gold = {
        "S": _int(raw_gold.get("nord"), "gold.nord", 0, MAX_GOLD),
        "R": _int(raw_gold.get("ember"), "gold.ember", 0, MAX_GOLD),
    }

    turn = _int(raw.get("turn"), "turn", 1, MAX_TURN)

    grid = {}
    for name in pc.TILE_NAMES:
        cell = board.get(name)
        if not isinstance(cell, dict):
            raise GameStateError("tile %s must be an object" % name)
        terrain_raw = cell.get("terrain")
        if not isinstance(terrain_raw, str):
            raise GameStateError("tile %s has an unknown terrain" % name)
        terrain = PAGE_TERRAIN_TO_ENGINE.get(terrain_raw)
        if terrain is None:
            raise GameStateError("tile %s has an unknown terrain" % name)

        owner_page = _owner(cell.get("owner"), "tile %s owner" % name)
        owner = PAGE_OWNER_TO_ENGINE.get(owner_page) if owner_page in PAGE_OWNER_TO_ENGINE else None
        armies = _int(cell.get("armies", 0), "tile %s armies" % name, 0, MAX_ARMIES)

        town = None
        capital = None
        if terrain == "town":
            town_name = cell.get("name") or DEFAULT_TOWN_NAMES.get(name) or name
            if not isinstance(town_name, str) or not town_name.strip():
                raise GameStateError("tile %s town name is invalid" % name)
            town_name = town_name.strip()[:MAX_NAME_LEN]
            militia = 0
            if owner is None or owner == "neutral":
                # The page puts neutral militia in `armies` with owner
                # "neutral"; the engine keeps them in town.militia and requires
                # a neutral town to carry no owner and no standing army.
                militia = armies
                armies = 0
                owner = None
            town = {"name": town_name, "militia": militia}
        elif terrain == "capital":
            capital = CAPITAL_SIDE[name]
            if owner is None:
                raise GameStateError("capital %s cannot be ownerless" % name)
        elif owner is None and armies > 0:
            raise GameStateError("tile %s has armies without an owner" % name)

        ready = cell.get("ready", armies)
        ready = _int(ready, "tile %s ready" % name, 0, armies) if armies else 0
        immobile = armies - ready
        moved = bool(armies > 0 and ready == 0)
        built_armies = 0 if moved else immobile

        grid[name] = {
            "terrain": terrain,
            "owner": owner,
            "town": town,
            "capital": capital,
            "armies": armies,
            "moved": moved,
            "built": built_armies > 0,
            "built_armies": built_armies,
        }

    try:
        return pc.normalise_position({
            "side": side,
            "gold": gold,
            "turn": turn,
            "grid": grid,
            "captured_this_turn": [],
        })
    except ValueError as exc:
        raise GameStateError(str(exc))


def page_state_from_engine(position):
    """Convert an engine position into the shape the page's `adoptState()` reads."""
    side = position["side"]
    ply = 2 * position["turn"] - (1 if side == "S" else 0)
    win = pc.winner(position)
    tiles = {}
    for name in pc.TILE_NAMES:
        tile = position["grid"][name]
        terrain = tile["terrain"]
        owner = tile["owner"]
        armies = tile["armies"]
        immobile = 0
        if terrain == "town" and owner is None and tile["town"] and tile["town"]["militia"] > 0:
            owner_page = "neutral"
            armies = tile["town"]["militia"]
        else:
            owner_page = ENGINE_OWNER_TO_PAGE.get(owner)
            immobile = armies if tile["moved"] else tile.get("built_armies", 0)
            immobile = max(0, min(immobile, armies))
        tiles[name] = {
            "id": name,
            "terrain": ENGINE_TERRAIN_TO_PAGE.get(terrain, terrain),
            "name": (tile["town"]["name"] if tile["town"] else "") or "",
            "owner": owner_page,
            "armies": armies,
            "immobile": immobile,
            "acquiredPly": ply if name in position.get("captured_this_turn", []) else 0,
        }
    return {
        "game": "pocket-conquest",
        "turn": ENGINE_OWNER_TO_PAGE.get(side),
        "ply": ply,
        "gold": {"nord": position["gold"]["S"], "ember": position["gold"]["R"]},
        "winner": ENGINE_OWNER_TO_PAGE.get(win) if win else None,
        "tiles": tiles,
    }


class GameStore:
    """Bounded, in-memory game table: it only has to live as long as the service."""

    def __init__(self, max_games=MAX_GAMES, ttl=GAME_TTL_SECONDS):
        self.max_games = max_games
        self.ttl = ttl
        self.lock = threading.Lock()
        self.games = {}

    def _prune_locked(self, now):
        stale = [gid for gid, entry in self.games.items() if now - entry["touched"] > self.ttl]
        for gid in stale:
            self.games.pop(gid, None)
        while len(self.games) >= self.max_games:
            oldest = min(self.games.items(), key=lambda item: item[1]["touched"])[0]
            self.games.pop(oldest, None)

    def create(self, position):
        now = time.time()
        with self.lock:
            self._prune_locked(now)
            gid = secrets.token_hex(8)
            while gid in self.games:
                gid = secrets.token_hex(8)
            self.games[gid] = {"position": position, "touched": now, "moves": 0}
            return gid

    def adopt(self, gid, position):
        """Store a state under an id we did not mint.

        The page resends its whole state on every move, so a game is fully
        recoverable after a service restart; the id is re-adopted rather than
        losing the game to a 404. The table stays bounded by _prune_locked.
        """
        now = time.time()
        if not valid_game_id(gid):
            return False
        with self.lock:
            self._prune_locked(now)
            if gid in self.games:
                return False
            self.games[gid] = {"position": position, "touched": now, "moves": 0}
            return True

    def get(self, gid):
        now = time.time()
        with self.lock:
            entry = self.games.get(gid)
            if entry is None:
                return None
            entry["touched"] = now
            return entry["position"]

    def put(self, gid, position):
        now = time.time()
        with self.lock:
            entry = self.games.get(gid)
            if entry is None:
                return False
            entry["position"] = position
            entry["touched"] = now
            entry["moves"] += 1
            return True

    def count(self):
        with self.lock:
            return len(self.games)


def valid_game_id(gid):
    """Accept exactly the shape `create()` mints: 16 lowercase hex characters.

    Anything else is refused, so a caller-supplied id lives in the same 64-bit
    space as a minted one and cannot be used to pre-occupy a short or guessable
    name.
    """
    if not isinstance(gid, str) or len(gid) != 16:
        return False
    return all(ch in "0123456789abcdef" for ch in gid)


def selftest():
    """Round-trip and turn-flow checks; returns the number of failures."""
    failures = []

    def check(name, fn):
        try:
            fn()
        except Exception as exc:  # noqa: BLE001 - selftest reports everything
            failures.append("%s: %r" % (name, exc))

    def page_payload(ply, turn_owner):
        pos = pc.initial_position()
        if turn_owner == "ember":
            pos["side"] = "R"
            pos["turn"] = 1
        page = page_state_from_engine(pos)
        assert page["ply"] == (2 if turn_owner == "ember" else 1), page["ply"]
        # The page's own payload has no immobile/acquiredPly; feed it its shape.
        board = {
            name: {
                "terrain": page["tiles"][name]["terrain"],
                "name": page["tiles"][name]["name"],
                "owner": page["tiles"][name]["owner"],
                "armies": page["tiles"][name]["armies"],
                "ready": page["tiles"][name]["armies"] - page["tiles"][name]["immobile"],
            }
            for name in pc.TILE_NAMES
        }
        return {
            "game": "pocket-conquest",
            "you": turn_owner,
            "turn": (page["ply"] + 1) // 2,
            "gold": page["gold"],
            "board": board,
        }

    def test_roundtrip_initial():
        payload = page_payload(1, "nord")
        pos = engine_position_from_page(payload, "S")
        assert pos["gold"]["S"] == 10 and pos["gold"]["R"] == 10
        assert pos["grid"]["B5"]["owner"] == "S" and pos["grid"]["B5"]["armies"] == 3
        assert pos["grid"]["E1"]["capital"] == "R"
        for name, militia in (("D1", 1), ("B3", 2), ("E5", 1)):
            assert pos["grid"][name]["owner"] is None
            assert pos["grid"][name]["town"]["militia"] == militia
        back = page_state_from_engine(pos)
        assert back["turn"] == "nord" and back["ply"] == 1
        assert back["tiles"]["D1"]["owner"] == "neutral" and back["tiles"]["D1"]["armies"] == 1

    def test_partial_stack_stays_partly_movable():
        payload = page_payload(3, "nord")
        payload["board"]["B5"] = {"terrain": "capital", "name": "Nordhold",
                                  "owner": "nord", "armies": 5, "ready": 2}
        pos = engine_position_from_page(payload, "S")
        assert pc._movable_armies(pos["grid"]["B5"]) == 2, pos["grid"]["B5"]
        assert not pos["grid"]["B5"]["moved"]
        back = page_state_from_engine(pos)
        assert back["tiles"]["B5"]["immobile"] == 3, back["tiles"]["B5"]

    def test_juno_turn_returns_the_page():
        payload = page_payload(2, "ember")
        pos = engine_position_from_page(payload, "R")
        legal = pc.legal_moves(pos)
        assert legal, "ember must have legal moves"
        move = legal[0]
        pos = pc.apply_move(pos, move)
        pos = pc.apply_move(pos, "END")
        back = page_state_from_engine(pos)
        assert back["turn"] == "nord", back["turn"]
        assert back["ply"] == 3, back["ply"]

    def test_rejects_bad_board():
        payload = page_payload(1, "nord")
        payload["board"].pop("A1")
        try:
            engine_position_from_page(payload, "S")
        except GameStateError:
            return
        raise AssertionError("a 29-tile board was accepted")

    def test_rejects_bad_owner():
        payload = page_payload(1, "nord")
        payload["board"]["A1"]["owner"] = "pirates"
        payload["board"]["A1"]["armies"] = 1
        try:
            engine_position_from_page(payload, "S")
        except GameStateError:
            return
        raise AssertionError("an unknown owner was accepted")

    def test_store_bounded():
        store = GameStore(max_games=3)
        ids = [store.create(pc.initial_position()) for _ in range(5)]
        assert store.count() == 3, store.count()
        assert store.get(ids[-1]) is not None

    for name, fn in (
        ("test_roundtrip_initial", test_roundtrip_initial),
        ("test_partial_stack_stays_partly_movable", test_partial_stack_stays_partly_movable),
        ("test_juno_turn_returns_the_page", test_juno_turn_returns_the_page),
        ("test_rejects_bad_board", test_rejects_bad_board),
        ("test_rejects_bad_owner", test_rejects_bad_owner),
        ("test_store_bounded", test_store_bounded),
    ):
        check(name, fn)
    for message in failures:
        print("FAIL " + message)
    print("game_state selftest: %d failures" % len(failures))
    return len(failures)


if __name__ == "__main__":
    raise SystemExit(1 if selftest() else 0)
