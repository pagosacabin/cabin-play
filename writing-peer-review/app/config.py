import os
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent.parent
STATIC_DIR = BASE_DIR / "static"
REVIEW_PROMPT_PATH = BASE_DIR / "review_prompt.md"
DISCUSSION_PROMPT_PATH = BASE_DIR / "discussion_prompt.md"

DEFAULT_DISPLAY_NAME = "Bethany"
DEVELOP_PROMPT_PATH = BASE_DIR / "develop_prompt.md"
STUCK_PROMPT_PATH = BASE_DIR / "stuck_prompt.md"


def load_env_file(env_path: Path) -> dict[str, str]:
    """Parse a simple .env file without external dependencies."""
    env_vars: dict[str, str] = {}
    if not env_path.is_file():
        return env_vars
    try:
        with open(env_path, "r", encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#"):
                    continue
                if "=" in line:
                    key, val = line.split("=", 1)
                    key = key.strip()
                    val = val.strip()
                    if (val.startswith('"') and val.endswith('"')) or (
                        val.startswith("'") and val.endswith("'")
                    ):
                        val = val[1:-1]
                    env_vars[key] = val
    except Exception:
        pass
    return env_vars


# Load .env into os.environ if not already present
_loaded_env = load_env_file(BASE_DIR / ".env")
for _k, _v in _loaded_env.items():
    if _k not in os.environ:
        os.environ[_k] = _v


def get_db_path() -> Path:
    """Return the SQLite database path.

    Checks WPR_DB environment variable at call time, defaulting to data/app.db.
    Ensures the parent directory exists.
    """
    custom = os.environ.get("WPR_DB")
    if custom:
        path = Path(custom).resolve()
    else:
        path = (BASE_DIR / "data" / "app.db").resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    return path


def get_deepseek_api_key() -> str:
    """Return the DeepSeek API key without ever logging or exposing it."""
    return os.environ.get("DEEPSEEK_API_KEY", "").strip()


def get_deepseek_model() -> str:
    """Return configured DeepSeek model name, default 'deepseek-chat'."""
    return os.environ.get("DEEPSEEK_MODEL", "deepseek-chat").strip() or "deepseek-chat"
