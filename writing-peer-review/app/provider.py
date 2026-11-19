import json
import logging
import re
from typing import Any
import httpx

from app.config import (
    DEVELOP_PROMPT_PATH,
    DISCUSSION_PROMPT_PATH,
    REVIEW_PROMPT_PATH,
    STUCK_PROMPT_PATH,
    get_deepseek_api_key,
    get_deepseek_model,
)

logger = logging.getLogger(__name__)

STUB_REVIEW_TEMPLATE = """**STUB REVIEW** (DeepSeek API key missing or request could not complete)

## What worked
This is an automated fallback review demonstrating the peer-review feedback structure. Your piece establishes immediate sensory focus, particularly in how the physical environment anchors the narrative tone. For instance, the opening progression effectively signals character interiority without rushing into excessive backstory.

## Craft points
1. **Sensory Grounding**: Deepen the scene by balancing visual cues with auditory or tactile elements (e.g., changes in temperature, ambient background noise).
2. **Pacing and Dramatic Beats**: After pivotal lines of dialogue or revelation, give the characters a physical beat to breathe before moving to the next beat.

## A question for you
What is the unspoken risk or consequence your protagonist is trying hardest to avert in this moment?

## Follow-up exercise
Write a 100-word follow-up scene set five minutes later in the same space, focusing entirely on silence and physical objects without introducing any dialogue.

## Voice check
Reads human.
This is an automated fallback stub, not a live read: the band line above is a placeholder
for the section's shape, not a verdict on your piece. With an API key configured, your peer
reviewer gives a real ear for whether passages read human or machine-made, without metrics
or scores.
"""

STUB_DISCUSSION_TEMPLATE = """**STUB DISCUSSION** (DeepSeek API key missing or request could not complete)

Thank you for discussing this review with me. As your writing-group peer, I've re-read your piece alongside my earlier notes. While operating in offline fallback mode without an active API connection, I cannot generate dynamic answers to your specific craft questions, but I stand by our discussion on pacing, sensory details, and narrative tension. Configure a live key to discuss specific lines directly.
"""

VOICE_BANDS = ("Reads human.", "Mixed.", "Reads machine-made.")

# The heading is only a heading when it stands alone on its line — a review may mention
# "## Voice check" inside a sentence, and the normaliser must not act on that mention.
_VOICE_HEADING_RE = re.compile(r"(?m)^[ \t]*##[ \t]*Voice check[ \t]*$")


def normalize_voice_check(text: str) -> str:
    """Put the voice-check band on its own line.

    The prompt asks for the band as the first line of the section, but a live model
    occasionally continues the explanation on the same line ("Reads machine-made. A few
    passages carry ..."). Split that case so the band is always a standalone line; leave
    well-formed text completely untouched.
    """
    match = _VOICE_HEADING_RE.search(text)
    if not match:
        return text
    start = match.end()
    lines = text[start:].split("\n")
    # First non-empty line after the heading
    for i, line in enumerate(lines):
        stripped = line.strip()
        if not stripped:
            continue
        for band in VOICE_BANDS:
            if stripped == band:
                return text
            if stripped.startswith(band) and stripped[len(band):len(band) + 1] in (" ", "*", "_"):
                rest = stripped[len(band):].strip()
                # Drop a list marker if the band opened a bullet; keep real markdown emphasis.
                if rest.startswith("* ") or rest.startswith("- "):
                    rest = rest[2:].strip()
                new_lines = lines[:i] + [band, "", rest] + lines[i + 1:]
                return text[:start] + "\n".join(new_lines)
        return text
    return text



def load_system_prompt(exercise_prompt: str | None = None) -> str:
    """Read review_prompt.md at call time and append exercise prompt if present."""
    try:
        base_prompt = REVIEW_PROMPT_PATH.read_text(encoding="utf-8").strip()
    except Exception:
        base_prompt = (
            "You are a thoughtful, observant writing-group peer reviewing a creative writing piece.\n\n"
            "Return feedback using EXACTLY these five markdown headings:\n"
            "## What worked\n## Craft points\n## A question for you\n## Follow-up exercise\n## Voice check\n\n"
            "The first line of ## Voice check must be exactly one of: Reads human. / Mixed. / Reads machine-made.\n"
        )

    if exercise_prompt and exercise_prompt.strip():
        base_prompt += f"\n\n### Original Writing Exercise\n{exercise_prompt.strip()}"

    return base_prompt


def review_piece(piece_text: str, exercise_prompt: str | None = None) -> tuple[str, str]:
    """Review a piece of creative writing using DeepSeek or fall back to canned stub review.

    Returns:
        tuple[str, str]: (review_text, provider_name) where provider_name is
                         'deepseek' or 'stub'.
    """
    api_key = get_deepseek_api_key()
    if not api_key:
        logger.warning("No DeepSeek API key configured; falling back to stub review.")
        return (STUB_REVIEW_TEMPLATE.strip(), "stub")

    system_prompt = load_system_prompt(exercise_prompt)
    model = get_deepseek_model()
    url = "https://api.deepseek.com/chat/completions"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": system_prompt},
            {"role": "user", "content": piece_text},
        ],
        "temperature": 0.7,
        "max_tokens": 1200,
    }

    try:
        with httpx.Client(timeout=120.0) as client:
            response = client.post(url, headers=headers, json=payload)
            response.raise_for_status()
            data = response.json()
            choices = data.get("choices")
            if choices and len(choices) > 0:
                review_content = choices[0].get("message", {}).get("content", "").strip()
                if review_content:
                    return (normalize_voice_check(review_content), "deepseek")
            # If empty choice
            return (STUB_REVIEW_TEMPLATE.strip(), "stub")
    except Exception as exc:
        # Never log or expose the API key
        err_msg = f"{exc.__class__.__name__}: {exc}"
        logger.warning("DeepSeek review request failed (%s); returning stub review.", err_msg)
        stub_with_error = (
            f"**STUB REVIEW** (DeepSeek call failed: {exc.__class__.__name__})\n\n"
            + STUB_REVIEW_TEMPLATE.partition("\n\n")[2]
        )
        return (stub_with_error.strip(), "stub")


def load_discussion_prompt(exercise_prompt: str | None = None) -> str:
    """Read discussion_prompt.md at call time and append exercise prompt if present."""
    try:
        base_prompt = DISCUSSION_PROMPT_PATH.read_text(encoding="utf-8").strip()
    except Exception:
        base_prompt = (
            "You are the peer who wrote this review. Discuss this piece and this review — nothing generic.\n"
            "Answer her questions, explain your reasoning, point at her own lines when you make a claim.\n"
            "You may illustrate a point with a short example (a sentence, a paragraph) when she asks or when it genuinely clarifies — never rewrite the whole piece uninvited.\n"
            "Peer, not teacher: honest, specific, no empty praise. If she convinces you a point was wrong, concede it plainly — a peer can change their mind.\n"
            "If she asks about the voice check, discuss it the same way — point at her lines, and concede plainly if she convinces you a passage reads human after all."
        )

    if exercise_prompt and exercise_prompt.strip():
        base_prompt += f"\n\n### Original Writing Exercise\n{exercise_prompt.strip()}"

    return base_prompt


def discuss_review(
    piece_text: str,
    review_text: str,
    thread: list[dict[str, Any]],
    exercise_prompt: str | None = None,
) -> tuple[str, str]:
    """Discuss a review and piece using DeepSeek or fall back to canned stub reply.

    Returns:
        tuple[str, str]: (reply_text, provider_name) where provider_name is
                         'deepseek' or 'stub'.
    """
    api_key = get_deepseek_api_key()
    if not api_key:
        logger.warning("No DeepSeek API key configured; falling back to stub discussion.")
        return (STUB_DISCUSSION_TEMPLATE.strip(), "stub")

    system_prompt = load_discussion_prompt(exercise_prompt)
    model = get_deepseek_model()
    url = "https://api.deepseek.com/chat/completions"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    # Context sent to model: discussion system prompt, piece text, review text, thread so far
    system_content = (
        f"{system_prompt}\n\n"
        f"## Piece Under Discussion\n{piece_text.strip()}\n\n"
        f"## Peer Review Under Discussion\n{review_text.strip()}"
    )

    messages: list[dict[str, str]] = [{"role": "system", "content": system_content}]
    for msg in thread:
        role = msg.get("role", "user")
        if role not in ("user", "assistant"):
            role = "user"
        text = msg.get("text", "") or msg.get("content", "")
        messages.append({"role": role, "content": text})

    payload = {
        "model": model,
        "messages": messages,
        "temperature": 0.7,
        "max_tokens": 1200,
    }

    try:
        with httpx.Client(timeout=120.0) as client:
            response = client.post(url, headers=headers, json=payload)
            response.raise_for_status()
            data = response.json()
            choices = data.get("choices")
            if choices and len(choices) > 0:
                reply_content = choices[0].get("message", {}).get("content", "").strip()
                if reply_content:
                    return (reply_content, "deepseek")
            return (STUB_DISCUSSION_TEMPLATE.strip(), "stub")
    except Exception as exc:
        # Never log or expose the API key
        err_msg = f"{exc.__class__.__name__}: {exc}"
        logger.warning("DeepSeek discussion request failed (%s); returning stub discussion.", err_msg)
        stub_with_error = (
            f"**STUB DISCUSSION** (DeepSeek call failed: {exc.__class__.__name__})\n\n"
            + STUB_DISCUSSION_TEMPLATE.partition("\n\n")[2]
        )
        return (stub_with_error.strip(), "stub")


# ---------------------------------------------------------------------------
# STUB templates for the new Develop mode functions
# ---------------------------------------------------------------------------

STUB_DEVELOP_TEMPLATE = (
    "**STUB DEVELOP REPLY** (DeepSeek API key missing or request could not complete)\n\n"
    "That's an interesting direction to explore. What made you land on that particular choice? "
    "Looking at what you have so far, the tension between the characters feels like the core "
    "thread to follow. I'd want to understand more about what you're trying to hold onto before "
    "suggesting we pull it in a new direction. What's the moment in the manuscript that you're "
    "least sure about right now?"
)

STUB_STUCK_TEMPLATE = (
    "1. What does the character want most in this moment, and what's stopping them?\n"
    "2. There's a detail from earlier in the piece — bring it back here.\n"
    "3. What question does this scene need to answer before it can end?"
)


def _render_bible_text(bible: list[dict[str, Any]]) -> str:
    """Render bible entries as grouped plain text for the system prompt."""
    if not bible:
        return "(no entries yet)"
    groups: dict[str, list[str]] = {}
    for entry in bible:
        kind = entry.get("kind", "other")
        name = entry.get("name", "")
        detail = entry.get("detail", "")
        groups.setdefault(kind, []).append(f"- {name}: {detail}" if name else f"- {detail}")
    parts = []
    for kind, lines in groups.items():
        parts.append(f"### {kind.capitalize()}\n" + "\n".join(lines))
    return "\n\n".join(parts)


def _build_develop_messages(
    system_content: str,
    thread_messages: list[dict[str, Any]],
    max_chars: int = 60000,
) -> list[dict[str, str]]:
    """Build the chat message list, dropping oldest non-first messages when too long."""
    chat: list[dict[str, str]] = [{"role": "system", "content": system_content}]
    for msg in thread_messages:
        role = msg.get("role", "user")
        if role not in ("user", "assistant"):
            role = "user"
        text = msg.get("text", "") or msg.get("content", "")
        chat.append({"role": role, "content": text})

    # Trim oldest messages (keep system + first user + recent) if too long
    total_chars = sum(len(m["content"]) for m in chat)
    if total_chars > max_chars and len(chat) > 3:
        # Always keep: system (index 0), first user message (index 1 if user)
        # Drop from index 2 onward (oldest non-first messages)
        keep_head = chat[:2]
        tail = chat[2:]
        while tail and sum(len(m["content"]) for m in keep_head + tail) > max_chars:
            tail.pop(0)
        chat = keep_head + tail
    return chat


def _render_thread_transcript(
    thread_messages: list[dict[str, Any]], max_chars: int = 60000
) -> str:
    """Render a develop thread as plain text for an extraction call.

    Extraction calls must not hand the thread back as chat turns: a thread that
    ends with an assistant reply makes the model continue the conversation
    instead of answering the instruction that follows (seen live 2026-11-19 —
    propose-bible returned prose, not JSON). One user message carrying the
    transcript *and* the instruction keeps the instruction last.
    """
    lines = []
    for msg in thread_messages:
        role = msg.get("role", "user")
        speaker = "Bethany" if role == "user" else "Partner"
        text = (msg.get("text") or msg.get("content") or "").strip()
        if text:
            lines.append(f"{speaker}: {text}")
    transcript = "\n\n".join(lines)
    if len(transcript) > max_chars:
        transcript = transcript[-max_chars:]
    return transcript


def develop_reply(
    piece_text: str,
    bible: list[dict[str, Any]],
    thread_messages: list[dict[str, Any]],
) -> tuple[str, str]:
    """Call the development-editor with the piece, bible, and thread history.

    Returns (reply_text, provider_name) where provider_name is 'deepseek' or 'stub'.
    -- STUB FALLBACK clearly marked below --
    """
    api_key = get_deepseek_api_key()
    if not api_key:
        logger.warning("No DeepSeek API key configured; falling back to stub develop reply.")
        return (STUB_DEVELOP_TEMPLATE.strip(), "stub")  # STUB

    try:
        base_prompt = DEVELOP_PROMPT_PATH.read_text(encoding="utf-8").strip()
    except Exception:
        base_prompt = "You are a trusted development-editor peer."

    bible_text = _render_bible_text(bible)
    system_content = (
        f"{base_prompt}\n\n"
        f"## The manuscript\n{piece_text.strip()}\n\n"
        f"## Story bible\n{bible_text}"
    )

    model = get_deepseek_model()
    url = "https://api.deepseek.com/chat/completions"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    messages = _build_develop_messages(system_content, thread_messages)
    payload = {
        "model": model,
        "messages": messages,
        "temperature": 0.8,
        "max_tokens": 700,
    }

    try:
        with httpx.Client(timeout=120.0) as client:
            response = client.post(url, headers=headers, json=payload)
            response.raise_for_status()
            data = response.json()
            choices = data.get("choices")
            if choices and len(choices) > 0:
                content = choices[0].get("message", {}).get("content", "").strip()
                if content:
                    return (content, "deepseek")
            return (STUB_DEVELOP_TEMPLATE.strip(), "stub")  # STUB
    except Exception as exc:
        err_msg = f"{exc.__class__.__name__}: {exc}"
        logger.warning("DeepSeek develop_reply failed (%s); returning stub.", err_msg)
        return (STUB_DEVELOP_TEMPLATE.strip(), "stub")  # STUB


def propose_bible_entries(
    piece_text: str,
    bible: list[dict[str, Any]],
    thread_messages: list[dict[str, Any]],
) -> tuple[list[dict[str, Any]], str]:
    """Ask the model to propose bible entries from the thread.

    Returns (list_of_entry_dicts, provider_name). Writes nothing to the DB.
    -- STUB FALLBACK clearly marked below --
    """
    api_key = get_deepseek_api_key()
    if not api_key:
        logger.warning("No DeepSeek API key configured; returning empty bible proposals (stub).")
        return ([], "stub")  # STUB

    try:
        base_prompt = DEVELOP_PROMPT_PATH.read_text(encoding="utf-8").strip()
    except Exception:
        base_prompt = "You are a trusted development-editor peer."

    bible_text = _render_bible_text(bible)
    system_content = (
        f"{base_prompt}\n\n"
        f"## The manuscript\n{piece_text.strip()}\n\n"
        f"## Story bible\n{bible_text}"
    )
    user_content = (
        "## The conversation so far\n"
        f"{_render_thread_transcript(thread_messages)}\n\n"
        "---\n"
        "Identify the facts, decisions, and character details that were clearly agreed upon "
        "in that conversation. Reply with STRICT JSON and nothing else, no markdown fences:\n"
        '{"entries":[{"kind":"character|timeline|world|decision","name":"...","detail":"..."}]}\n'
        "Include 1 to 6 entries capturing only what was settled. "
        "If nothing was settled, reply {\"entries\":[]}."
    )

    model = get_deepseek_model()
    url = "https://api.deepseek.com/chat/completions"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    messages = [
        {"role": "system", "content": system_content},
        {"role": "user", "content": user_content},
    ]
    payload = {
        "model": model,
        "messages": messages,
        "temperature": 0.3,
        "max_tokens": 700,
    }

    try:
        with httpx.Client(timeout=120.0) as client:
            response = client.post(url, headers=headers, json=payload)
            response.raise_for_status()
            data = response.json()
            choices = data.get("choices")
            if choices and len(choices) > 0:
                raw = choices[0].get("message", {}).get("content", "").strip()
                # Strip markdown fences defensively
                raw = re.sub(r"^```[a-z]*\n?", "", raw, flags=re.IGNORECASE)
                raw = re.sub(r"\n?```$", "", raw)
                raw = raw.strip()
                try:
                    parsed = json.loads(raw)
                    entries = parsed.get("entries", [])
                    if isinstance(entries, list):
                        # Validate each entry has required keys
                        valid = []
                        for e in entries:
                            if isinstance(e, dict) and "kind" in e and "name" in e and "detail" in e:
                                valid.append({
                                    "kind": str(e["kind"]),
                                    "name": str(e["name"]),
                                    "detail": str(e["detail"]),
                                })
                        return (valid, "deepseek")
                except (json.JSONDecodeError, ValueError):
                    logger.warning("propose_bible_entries: JSON parse failed; returning empty list.")
                    return ([], "deepseek")
            return ([], "stub")  # STUB
    except Exception as exc:
        err_msg = f"{exc.__class__.__name__}: {exc}"
        logger.warning("DeepSeek propose_bible_entries failed (%s); returning empty list.", err_msg)
        return ([], "stub")  # STUB


def draft_merge(
    piece_text: str,
    bible: list[dict[str, Any]],
    thread_messages: list[dict[str, Any]],
    insert_after_paragraph: Any,
) -> tuple[dict[str, Any], str]:
    """Ask the model to draft a merge: bible entries and an optional passage.

    insert_after_paragraph is "end" or an integer paragraph index (0-based).
    Returns (dict_with_bible_entries_and_passage, provider_name).
    Writes nothing to the DB.
    -- STUB FALLBACK clearly marked below --
    """
    api_key = get_deepseek_api_key()
    if not api_key:
        logger.warning("No DeepSeek API key configured; returning stub merge preview.")
        return ({"bible_entries": [], "passage": ""}, "stub")  # STUB

    try:
        base_prompt = DEVELOP_PROMPT_PATH.read_text(encoding="utf-8").strip()
    except Exception:
        base_prompt = "You are a trusted development-editor peer."

    bible_text = _render_bible_text(bible)

    if insert_after_paragraph == "end":
        position_desc = "at the end of the manuscript"
    else:
        position_desc = f"after paragraph {insert_after_paragraph} (0-based) of the manuscript"

    system_content = (
        f"{base_prompt}\n\n"
        f"## The manuscript\n{piece_text.strip()}\n\n"
        f"## Story bible\n{bible_text}"
    )
    user_content = (
        "## The conversation so far\n"
        f"{_render_thread_transcript(thread_messages)}\n\n"
        "---\n"
        "Draft what this conversation would add to the manuscript. Reply with STRICT JSON and "
        "nothing else, no markdown fences:\n"
        '{"bible_entries":[{"kind":"character|timeline|world|decision","name":"...","detail":"..."}],"passage":"..."}\n'
        f"The passage is 60 to 120 words of finished prose, drafted to sit {position_desc}. It must "
        "embody the decision or development the conversation actually reached \u2014 return an empty "
        "passage only if the conversation reached no decision and changed nothing in the story. "
        "bible_entries captures the settled facts behind it (may be an empty list). "
        "Reply with only the JSON object."
    )

    model = get_deepseek_model()
    url = "https://api.deepseek.com/chat/completions"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }

    messages = [
        {"role": "system", "content": system_content},
        {"role": "user", "content": user_content},
    ]
    payload = {
        "model": model,
        "messages": messages,
        "temperature": 0.4,
        "max_tokens": 700,
    }

    try:
        with httpx.Client(timeout=120.0) as client:
            response = client.post(url, headers=headers, json=payload)
            response.raise_for_status()
            data = response.json()
            choices = data.get("choices")
            if choices and len(choices) > 0:
                raw = choices[0].get("message", {}).get("content", "").strip()
                raw = re.sub(r"^```[a-z]*\n?", "", raw, flags=re.IGNORECASE)
                raw = re.sub(r"\n?```$", "", raw)
                raw = raw.strip()
                try:
                    parsed = json.loads(raw)
                    entries = parsed.get("bible_entries", [])
                    passage = str(parsed.get("passage", ""))
                    if isinstance(entries, list):
                        valid_entries = []
                        for e in entries:
                            if isinstance(e, dict) and "kind" in e and "name" in e and "detail" in e:
                                valid_entries.append({
                                    "kind": str(e["kind"]),
                                    "name": str(e["name"]),
                                    "detail": str(e["detail"]),
                                })
                        return ({"bible_entries": valid_entries, "passage": passage}, "deepseek")
                except (json.JSONDecodeError, ValueError):
                    logger.warning("draft_merge: JSON parse failed; returning empty result.")
                    return ({"bible_entries": [], "passage": ""}, "deepseek")
            return ({"bible_entries": [], "passage": ""}, "stub")  # STUB
    except Exception as exc:
        err_msg = f"{exc.__class__.__name__}: {exc}"
        logger.warning("DeepSeek draft_merge failed (%s); returning stub.", err_msg)
        return ({"bible_entries": [], "passage": ""}, "stub")  # STUB


def stuck_directions(piece_text: str) -> tuple[str, str]:
    """Call the model with stuck_prompt.md and return a numbered list of 2-3 directions.

    Returns (directions_text, provider_name) where provider_name is 'deepseek' or 'stub'.
    -- STUB FALLBACK clearly marked below --
    """
    api_key = get_deepseek_api_key()
    if not api_key:
        logger.warning("No DeepSeek API key configured; falling back to stub stuck directions.")
        return (STUB_STUCK_TEMPLATE.strip(), "stub")  # STUB

    try:
        stuck_prompt = STUCK_PROMPT_PATH.read_text(encoding="utf-8").strip()
    except Exception:
        stuck_prompt = (
            "You are a trusted writing peer. The writer is stuck. "
            "Return exactly 2 or 3 numbered directions (one sentence each) and nothing else."
        )

    model = get_deepseek_model()
    url = "https://api.deepseek.com/chat/completions"
    headers = {
        "Authorization": f"Bearer {api_key}",
        "Content-Type": "application/json",
    }
    payload = {
        "model": model,
        "messages": [
            {"role": "system", "content": stuck_prompt},
            {"role": "user", "content": piece_text.strip()},
        ],
        "temperature": 0.8,
        "max_tokens": 300,
    }

    try:
        with httpx.Client(timeout=60.0) as client:
            response = client.post(url, headers=headers, json=payload)
            response.raise_for_status()
            data = response.json()
            choices = data.get("choices")
            if choices and len(choices) > 0:
                content = choices[0].get("message", {}).get("content", "").strip()
                if content:
                    return (content, "deepseek")
            return (STUB_STUCK_TEMPLATE.strip(), "stub")  # STUB
    except Exception as exc:
        err_msg = f"{exc.__class__.__name__}: {exc}"
        logger.warning("DeepSeek stuck_directions failed (%s); returning stub.", err_msg)
        return (STUB_STUCK_TEMPLATE.strip(), "stub")  # STUB
