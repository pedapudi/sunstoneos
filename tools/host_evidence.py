"""Create strict artifact destinations and distinct evidence directories."""
from datetime import datetime, timezone
from pathlib import Path
import tempfile


def new_directory(requested):
    """Create the exact artifact path, refusing any existing file or directory."""
    requested = Path(requested)
    requested.mkdir(parents=True)
    return requested


def unique_directory(requested):
    """Use the requested directory, or a unique child when it already exists."""
    requested = Path(requested)
    try:
        requested.mkdir(parents=True)
        return requested
    except FileExistsError:
        if not requested.is_dir():
            raise
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ-")
    return Path(tempfile.mkdtemp(prefix=stamp, dir=requested))
