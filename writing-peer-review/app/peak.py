from datetime import datetime, time, timedelta, timezone


def is_utc_peak(dt: datetime) -> bool:
    """Return True if dt is within peak UTC hours (Mon-Fri 01:00-04:00 or 06:00-10:00 UTC).

    Weekends (Saturday, Sunday) are always off-peak.
    """
    if dt.tzinfo is None:
        dt = dt.replace(tzinfo=timezone.utc)
    else:
        dt = dt.astimezone(timezone.utc)

    # 0 = Monday, ..., 4 = Friday, 5 = Saturday, 6 = Sunday
    if dt.weekday() >= 5:
        return False

    t = dt.time()
    return (time(1, 0) <= t < time(4, 0)) or (time(6, 0) <= t < time(10, 0))


def compute_next_flip_utc(now_utc: datetime) -> datetime:
    """Compute the next UTC datetime when the peak status will flip."""
    if now_utc.tzinfo is None:
        now_utc = now_utc.replace(tzinfo=timezone.utc)
    else:
        now_utc = now_utc.astimezone(timezone.utc)

    current_state = is_utc_peak(now_utc)
    base_date = now_utc.date()

    candidates: list[datetime] = []
    # Transitions can only occur at 01:00, 04:00, 06:00, 10:00 UTC
    for day_offset in range(9):
        d = base_date + timedelta(days=day_offset)
        for h in (1, 4, 6, 10):
            cand = datetime(d.year, d.month, d.day, h, 0, 0, tzinfo=timezone.utc)
            if cand > now_utc:
                candidates.append(cand)
    candidates.sort()

    for cand in candidates:
        if is_utc_peak(cand) != current_state:
            return cand

    raise RuntimeError("Failed to compute next peak flip transition")


def format_local_time(dt_utc: datetime) -> str:
    """Format UTC datetime as a 12-hour clock string in host's local timezone (e.g. '6:00 PM')."""
    local_dt = dt_utc.astimezone()
    hour = local_dt.hour % 12 or 12
    minute = f"{local_dt.minute:02d}"
    ampm = local_dt.strftime("%p")
    return f"{hour}:{minute} {ampm}"


def peak_status(now_utc: datetime | None = None) -> dict:
    """Return the peak/off-peak status dictionary.

    Format:
    {
        "is_peak": bool,
        "label": "Peak" | "Off-peak",
        "next_flip_utc": "YYYY-MM-DDTHH:MM:SSZ",
        "next_flip_local": "6:00 PM",
        "detail": "<human string>"
    }
    """
    if now_utc is None:
        now_utc = datetime.now(timezone.utc)
    elif now_utc.tzinfo is None:
        now_utc = now_utc.replace(tzinfo=timezone.utc)
    else:
        now_utc = now_utc.astimezone(timezone.utc)

    is_peak = is_utc_peak(now_utc)
    flip_utc = compute_next_flip_utc(now_utc)
    flip_local = format_local_time(flip_utc)
    flip_utc_iso = flip_utc.strftime("%Y-%m-%dT%H:%M:%SZ")

    label = "Peak" if is_peak else "Off-peak"
    if is_peak:
        detail = (
            f"Peak hours active (01:00-04:00, 06:00-10:00 UTC M-F). "
            f"Flips to off-peak at {flip_local} local ({flip_utc.strftime('%H:%M')} UTC)."
        )
    else:
        detail = (
            f"Off-peak hours active (discounted API pricing). "
            f"Flips to peak at {flip_local} local ({flip_utc.strftime('%H:%M')} UTC)."
        )

    return {
        "is_peak": is_peak,
        "label": label,
        "next_flip_utc": flip_utc_iso,
        "next_flip_local": flip_local,
        "detail": detail,
    }
