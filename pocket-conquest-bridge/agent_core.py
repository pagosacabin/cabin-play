"""Pure-stdlib prompt builder and reply validator for Pocket Conquest AI."""

from __future__ import annotations

import re
import sys

import pocket_conquest as pc

_RULES_CONDENSED = (
    "RULES: 6x5 grid (A-F,1-5), orthogonal moves. Build: 5g/army at your capital or owned town "
    "(not captured this turn); built armies cannot move this turn. Move: 1 tile, split allowed, "
    "moved stacks stop (~=moved/built). Empty tile claimed; enemy or militia=combat: "
    "atk+1d6 vs def+1d6(+1 hill H), ties to def; margin>=3 loses 2 else 1; falls at 0 def. "
    "Pincer (+): adjacent stack adds armies, stays put. END collects gold (cap 5, town 4, "
    "plain/hill 2). Win by occupying enemy capital. "
    "STRATEGY: gold compounds -- grab a weak militia town early, keep your capital "
    "garrisoned, and only attack with a stack that outnumbers the defender."
)

_CANDIDATE_RE = re.compile(
    r"(?<![A-Za-z0-9_+])("
    r"M [A-F][1-5] [A-F][1-5] [1-9][0-9]* \+ [A-F][1-5] [1-9][0-9]*"
    r"|M [A-F][1-5] [A-F][1-5] [1-9][0-9]*"
    r"|B [A-F][1-5] [1-9][0-9]*"
    r"|END"
    r")(?![A-Za-z0-9_+])"
)


def _army_totals(position: dict) -> tuple[int, int]:
    """Return total armies on the board for ('S', 'R')."""
    s_armies = 0
    r_armies = 0
    for tile in position["grid"].values():
        if tile["owner"] == "S":
            s_armies += tile["armies"]
        elif tile["owner"] == "R":
            r_armies += tile["armies"]
    return s_armies, r_armies


def _tactics(position: dict, legal: list[str]) -> list[str]:
    """Name the concrete opportunities in `legal` so the model does not miss them.

    This is a hint list, not a decision: every entry is one of the legal moves
    and the model still chooses. Without it the model tends to build while a
    capital sits undefended next to its stack.
    """
    grid = position["grid"]
    side = position["side"]
    fatal: list[str] = []
    good: list[str] = []
    for move in legal:
        parsed = pc._parse_move(move)
        if not parsed or parsed[0] != "M":
            continue
        _, src, dst, count, sup, sup_count = parsed
        tile = grid[dst]
        if not pc._is_attack_target(tile, side):
            continue  # only real combats are worth a hint
        militia = tile["town"]["militia"] if tile["town"] else 0
        defenders = militia if militia > 0 else tile["armies"]
        bonus = 1 if tile["terrain"] == "hill" else 0
        attackers = count + sup_count
        if tile["capital"] not in (None, side) and tile["owner"] != side:
            if attackers >= defenders:
                fatal.append(f"{move} takes their capital and WINS the game")
        elif attackers >= defenders + 2 + bonus:
            what = "militia town" if militia else "stack"
            good.append(f"{move} is a favourable attack ({attackers} vs {defenders} {what})")
    hints = fatal[:2] + good[:2]
    return hints


def _tactics_block(position: dict, legal: list[str]) -> str:
    """Render the tactics hints as one prompt line (empty when there is nothing)."""
    hints = _tactics(position, legal)
    if not hints:
        return ""
    return "TACTICS: " + "; ".join(hints) + ".\n"


def build_prompt(position: dict, legal: list[str]) -> str:
    """Build the compact prompt containing rules, board, resources, and legal moves."""
    side = position["side"]
    gold = position["gold"]
    s_armies, r_armies = _army_totals(position)
    board = pc.describe(position)
    numbered = "\n".join(f"{idx}. {move}" for idx, move in enumerate(legal, start=1))
    return (
        f"{_RULES_CONDENSED}\n"
        f"Turn {position['turn']} | Side: {side} | "
        f"Gold S:{gold['S']} R:{gold['R']} | Armies S:{s_armies} R:{r_armies}\n"
        f"{board}\n"
        f"{_tactics_block(position, legal)}"
        f"Legal moves:\n{numbered}\n"
        f"If a TACTICS move is listed, prefer it. "
        f"Reply with EXACTLY ONE move string from the list above and nothing else. "
        f"Any reply not in the list will be rejected."
    )


def parse_reply(text: str, legal: list[str]) -> str | None:
    """Extract a single legal move from text, or return None if invalid or ambiguous."""
    if not isinstance(text, str) or not legal:
        return None
    stripped = text.strip()
    if not stripped:
        return None
    legal_set = set(legal)
    if stripped in legal_set:
        return stripped
    unquoted = stripped.strip("`'\"")
    if unquoted in legal_set:
        return unquoted

    # The prompt numbers the list, so a bare index is an unambiguous answer.
    index = stripped.rstrip(".")
    if index.isdigit():
        n = int(index)
        if index == str(n) and 1 <= n <= len(legal):  # "01" is not an index
            return legal[n - 1]
        return None

    line_matches: list[str] = []
    for line in stripped.splitlines():
        found = _CANDIDATE_RE.findall(line)
        if len(found) > 1:
            return None
        if len(found) == 1:
            candidate = found[0]
            if candidate not in legal_set:
                return None
            line_matches.append(candidate)

    unique = set(line_matches)
    if len(unique) == 1:
        return next(iter(unique))
    return None


def pick_legal(position: dict, model_call, retries: int = 2) -> tuple[str | None, list[dict]]:
    """Call model_call with retries until a legal move is parsed, or return (None, trace)."""
    legal = pc.legal_moves(position)
    if not legal:
        return None, [{"attempt": 0, "error": "no legal moves available"}]

    base_prompt = build_prompt(position, legal)
    prompt = base_prompt
    trace: list[dict] = []

    for attempt in range(1, retries + 2):
        try:
            raw = model_call(prompt)
        except Exception as exc:
            err = f"model_call raised {type(exc).__name__}: {exc}"
            trace.append({"attempt": attempt, "error": err})
            prompt = (
                f"{base_prompt}\n\n"
                f"Previous attempt failed ({err}). "
                f"Reply with ONLY one exact move string from the list."
            )
            continue

        move = parse_reply(raw, legal)
        if move is not None:
            return move, trace

        shown = repr(raw)
        snippet = shown if len(shown) <= 80 else shown[:77] + "..."
        err = f"unparseable or illegal reply: {snippet}"
        trace.append({"attempt": attempt, "error": err})
        prompt = (
            f"{base_prompt}\n\n"
            f"Previous reply was rejected ({err}). "
            f"Reply with ONLY one exact move string from the list."
        )

    return None, trace


def selftest() -> int:
    """Run agent_core self-tests with stub model calls and return failure count."""
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

    pos = pc.initial_position()
    legal = pc.legal_moves(pos)
    prompt = build_prompt(pos, legal)
    print(f"Prompt length (initial position): {len(prompt)} chars")

    def test_prompt_contents_and_length() -> None:
        assert len(prompt) <= 4000, f"Prompt too long: {len(prompt)}"
        assert "Gold S:10 R:10" in prompt
        assert "Armies S:3 R:3" in prompt
        assert "Side: S" in prompt
        assert "rejected" in prompt.lower()
        for m in legal:
            assert m in prompt

    def test_good_answer() -> None:
        calls: list[str] = []

        def stub(p: str) -> str:
            calls.append(p)
            return "M B5 B4 2"

        move, trace = pick_legal(pos, stub)
        assert move == "M B5 B4 2"
        assert trace == []
        assert len(calls) == 1

    def test_illegal_well_formed_retried_and_rejected() -> None:
        calls: list[str] = []

        def stub(p: str) -> str:
            calls.append(p)
            return "M B5 A1 1"  # well-formed grammar, illegal move

        move, trace = pick_legal(pos, stub, retries=2)
        assert move is None
        assert len(calls) == 3
        assert len(trace) == 3
        assert all("attempt" in entry and "error" in entry for entry in trace)
        assert "rejected" in calls[-1].lower()

    def test_extra_prose_accepted() -> None:
        def stub(_p: str) -> str:
            return "Thinking about options...\nI choose `B B5 2` for expansion."

        move, trace = pick_legal(pos, stub)
        assert move == "B B5 2"
        assert trace == []

    def test_empty_reply_rejected() -> None:
        def stub(_p: str) -> str:
            return "   \n  "

        move, trace = pick_legal(pos, stub, retries=1)
        assert move is None
        assert len(trace) == 2

    def test_exception_then_recovery() -> None:
        attempts = {"n": 0}

        def stub(_p: str) -> str:
            attempts["n"] += 1
            if attempts["n"] == 1:
                raise RuntimeError("transient timeout")
            return '"END"'

        move, trace = pick_legal(pos, stub, retries=2)
        assert move == "END"
        assert len(trace) == 1 and "RuntimeError" in trace[0]["error"]

    def test_bare_index_and_tactics() -> None:
        assert parse_reply("3", legal) == legal[2]
        assert parse_reply("99", legal) is None
        assert parse_reply("01", legal) is None  # a padded index is not an index
        siege = pc.initial_position()
        siege["grid"]["E2"] = {"terrain": "plain", "owner": "S", "armies": 6}
        siege["grid"]["E1"]["armies"] = 1
        siege["gold"] = {"S": 5, "R": 8}
        siege = pc.normalise_position(siege)
        text = build_prompt(siege, pc.legal_moves(siege))
        assert "WINS the game" in text, text
        assert "M E2 E1 6" in text, text
        # A move into an empty tile is never an "attack" hint.
        quiet = pc.initial_position()
        assert "TACTICS:" not in build_prompt(quiet, pc.legal_moves(quiet))

    check("prompt_contents_and_length", test_prompt_contents_and_length)
    check("bare_index_and_tactics", test_bare_index_and_tactics)
    check("good_answer", test_good_answer)
    check("illegal_well_formed_retried_and_rejected", test_illegal_well_formed_retried_and_rejected)
    check("extra_prose_accepted", test_extra_prose_accepted)
    check("empty_reply_rejected", test_empty_reply_rejected)
    check("exception_then_recovery", test_exception_then_recovery)

    print(f"agent_core selftest total failures: {failures}")
    return failures


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(1 if selftest() > 0 else 0)
