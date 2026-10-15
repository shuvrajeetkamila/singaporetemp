"""Source-manifest validation and release gate.

Every geographic input to the build must appear in a source register with
provider, license, dates, CRS, processing steps, and a confidence class.
This module validates the register and, with --release, refuses to pass
unless every source is approved for build with a verified license.

Usage:
    python -m sg_pipeline.manifest validate pipeline/manifests/source_register.json
    python -m sg_pipeline.manifest validate --release pipeline/manifests/source_register.json
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import re
import sys
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 1

CONFIDENCE_CLASSES = {
    "authoritative_survey",    # government survey / surveyed control
    "authoritative_vector",    # government vector product, date-stamped
    "licensed_lidar",          # licensed point cloud, usage rights confirmed
    "satellite_dem",           # regional or satellite DEM, accuracy recorded
    "osm_supplemental",        # OpenStreetMap, gap filling only
    "procedural_estimate",     # documented inference, not measured
    "unknown",                 # provenance incomplete
}

SOURCE_STATUSES = {
    "pending_terms_review",    # candidate; licence and field coverage not yet checked
    "approved_for_build",      # licence verified and processing documented
    "rejected",                # cannot be used (restricted, unlicensed, or unsuitable)
}

LICENSE_STATUSES = {"verified", "unverified"}

REQUIRED_FIELDS = (
    "id",
    "provider",
    "title",
    "license",
    "retrieved_on",
    "published_on",
    "crs",
    "vertical_datum",
    "confidence_class",
    "status",
    "processing_steps",
    "coverage",
    "notes",
)

_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{1,63}$")


def _parse_date(value: Any) -> _dt.date | None:
    if value is None:
        return None
    if not isinstance(value, str):
        raise ValueError("date must be an ISO-8601 string or null")
    return _dt.date.fromisoformat(value)


def validate_source(src: Any, release: bool) -> list[str]:
    """Return a list of human-readable problems for one source entry."""
    errors: list[str] = []
    if not isinstance(src, dict):
        return ["source entry is not an object"]

    sid = src.get("id", "<missing id>")
    prefix = f"[{sid}] "

    for field in REQUIRED_FIELDS:
        if field not in src:
            errors.append(prefix + f"missing field '{field}'")
    if errors:
        return errors

    if not isinstance(sid, str) or not _ID_RE.match(sid):
        errors.append(prefix + "id must match ^[a-z0-9][a-z0-9-]{1,63}$")

    lic = src["license"]
    if not isinstance(lic, dict):
        errors.append(prefix + "license must be an object")
        lic = {}
    for key in ("name", "status", "url"):
        if key not in lic:
            errors.append(prefix + f"license.{key} missing")
    if lic.get("status") not in LICENSE_STATUSES:
        errors.append(prefix + "license.status must be 'verified' or 'unverified'")

    try:
        _parse_date(src["retrieved_on"])
        _parse_date(src["published_on"])
    except ValueError as exc:
        errors.append(prefix + f"invalid date: {exc}")

    if src["confidence_class"] not in CONFIDENCE_CLASSES:
        errors.append(prefix + f"unknown confidence_class '{src['confidence_class']}'")

    if src["status"] not in SOURCE_STATUSES:
        errors.append(prefix + f"unknown status '{src['status']}'")

    if src["crs"] is not None and not (isinstance(src["crs"], str) and src["crs"].startswith("EPSG:")):
        errors.append(prefix + "crs must be an EPSG code string or null")

    coverage = src["coverage"]
    if coverage is not None:
        bbox = coverage.get("bbox_svy21") if isinstance(coverage, dict) else None
        if bbox is not None:
            if not (isinstance(bbox, list) and len(bbox) == 4 and all(isinstance(v, (int, float)) for v in bbox)):
                errors.append(prefix + "coverage.bbox_svy21 must be [minE, minN, maxE, maxN]")
            elif not (bbox[0] < bbox[2] and bbox[1] < bbox[3]):
                errors.append(prefix + "coverage.bbox_svy21 must be ordered")

    if src["status"] == "approved_for_build":
        if lic.get("status") != "verified":
            errors.append(prefix + "approved_for_build requires a verified license")
        url = lic.get("url")
        if not (isinstance(url, str) and url.startswith("https://")):
            errors.append(prefix + "approved_for_build requires an https license URL")
        if src["retrieved_on"] is None:
            errors.append(prefix + "approved_for_build requires retrieved_on")
        if src["crs"] is None:
            errors.append(prefix + "approved_for_build requires a CRS")
        if not src["processing_steps"]:
            errors.append(prefix + "approved_for_build requires processing_steps")
        if coverage is None:
            errors.append(prefix + "approved_for_build requires coverage")

    if release:
        if src["status"] != "approved_for_build":
            errors.append(prefix + f"release build blocked: status is '{src['status']}'")
        if lic.get("status") != "verified":
            errors.append(prefix + "release build blocked: license not verified")

    return errors


def validate_register(doc: Any, release: bool = False) -> list[str]:
    """Validate a whole register document. Returns a list of errors (empty when valid)."""
    if not isinstance(doc, dict):
        return ["register must be a JSON object"]
    errors: list[str] = []
    if doc.get("schema_version") != SCHEMA_VERSION:
        errors.append(f"schema_version must be {SCHEMA_VERSION}")
    sources = doc.get("sources")
    if not isinstance(sources, list) or not sources:
        return errors + ["sources must be a non-empty list"]

    seen: set[str] = set()
    for src in sources:
        if isinstance(src, dict) and isinstance(src.get("id"), str):
            if src["id"] in seen:
                errors.append(f"duplicate source id '{src['id']}'")
            seen.add(src["id"])
        errors.extend(validate_source(src, release))
    return errors


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="cmd", required=True)
    v = sub.add_parser("validate", help="validate a source register")
    v.add_argument("path", type=Path)
    v.add_argument("--release", action="store_true", help="fail unless every source is release-approved")
    args = parser.parse_args(argv)

    try:
        doc = json.loads(args.path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"error: cannot read register: {exc}", file=sys.stderr)
        return 2

    errors = validate_register(doc, release=args.release)
    if errors:
        for e in errors:
            print(e, file=sys.stderr)
        print(f"{len(errors)} problem(s) found", file=sys.stderr)
        return 1
    count = len(doc["sources"])
    mode = "release" if args.release else "schema"
    print(f"ok: {count} source(s) valid ({mode} check)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
