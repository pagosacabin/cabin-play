import os
from pathlib import Path
import pytest
from fastapi.testclient import TestClient


@pytest.fixture(scope="session", autouse=True)
def temp_db(tmp_path_factory):
    """Set up a temporary WPR_DB database for the pytest session."""
    db_file = tmp_path_factory.mktemp("wpr_data") / "test_app.db"
    old_wpr = os.environ.get("WPR_DB")
    os.environ["WPR_DB"] = str(db_file)
    yield db_file
    if old_wpr is not None:
        os.environ["WPR_DB"] = old_wpr
    else:
        os.environ.pop("WPR_DB", None)


@pytest.fixture
def client(temp_db):
    """Provide a TestClient using the session database."""
    from app.main import create_app

    app = create_app()
    with TestClient(app) as test_client:
        yield test_client
