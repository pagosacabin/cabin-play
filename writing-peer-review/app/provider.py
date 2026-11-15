import logging
import httpx

from app.config import (
    REVIEW_PROMPT_PATH,
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
"""


def load_system_prompt(exercise_prompt: str | None = None) -> str:
    """Read review_prompt.md at call time and append exercise prompt if present."""
    try:
        base_prompt = REVIEW_PROMPT_PATH.read_text(encoding="utf-8").strip()
    except Exception:
        base_prompt = (
            "You are a thoughtful, observant writing-group peer reviewing a creative writing piece.\n\n"
            "Return feedback using EXACTLY these four markdown headings:\n"
            "## What worked\n## Craft points\n## A question for you\n## Follow-up exercise\n"
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
                    return (review_content, "deepseek")
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
