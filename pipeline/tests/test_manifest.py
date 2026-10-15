import copy
import json
from pathlib import Path

import pytest

from sg_pipeline import manifest

REGISTER = Path(__file__).resolve().parents[1] / "manifests" / "source_register.json"


def _good_source():
    return {
        "id": "test-authoritative-vector",
        "provider": "Example Agency",
        "title": "Fixture source",
        "license": {"name": "Example Open Licence", "status": "verified", "url": "https://example.gov/licence"},
        "retrieved_on": "2026-10-01",
        "published_on": "2026-09-01",
        "crs": "EPSG:3414",
        "vertical_datum": "SVY21 height reference (to be confirmed)",
        "confidence_class": "authoritative_vector",
        "status": "approved_for_build",
        "processing_steps": ["reproject to EPSG:3414", "validate topology"],
        "coverage": {"bbox_svy21": [2500.0, 25000.0, 56000.0, 48000.0]},
        "notes": "fixture",
    }


def _doc(*sources):
    return {"schema_version": 1, "sources": list(sources)}


def test_candidate_register_is_schema_valid():
    doc = json.loads(REGISTER.read_text(encoding="utf-8"))
    assert manifest.validate_register(doc) == []


def test_candidate_register_fails_release_gate():
    doc = json.loads(REGISTER.read_text(encoding="utf-8"))
    errors = manifest.validate_register(doc, release=True)
    assert errors, "no candidate source may pass the release gate"
    assert all("release build blocked" in e for e in errors)


def test_good_source_passes_release_gate():
    assert manifest.validate_register(_doc(_good_source()), release=True) == []


def test_approved_source_without_verified_licence_rejected():
    src = _good_source()
    src["license"]["status"] = "unverified"
    errors = manifest.validate_register(_doc(src))
    assert any("requires a verified license" in e for e in errors)


def test_approved_source_requires_https_licence_url():
    src = _good_source()
    src["license"]["url"] = "http://example.gov/licence"
    assert any("https" in e for e in manifest.validate_register(_doc(src)))


def test_missing_field_reported():
    src = _good_source()
    del src["crs"]
    assert any("missing field 'crs'" in e for e in manifest.validate_register(_doc(src)))


def test_unknown_confidence_class_rejected():
    src = _good_source()
    src["confidence_class"] = "measured_by_vibes"
    assert any("unknown confidence_class" in e for e in manifest.validate_register(_doc(src)))


def test_bad_bbox_rejected():
    src = _good_source()
    src["coverage"] = {"bbox_svy21": [10.0, 10.0, 5.0, 20.0]}
    assert any("must be ordered" in e for e in manifest.validate_register(_doc(src)))


def test_bad_date_rejected():
    src = _good_source()
    src["retrieved_on"] = "2026-13-40"
    assert any("invalid date" in e for e in manifest.validate_register(_doc(src)))


def test_duplicate_ids_rejected():
    errors = manifest.validate_register(_doc(_good_source(), _good_source()))
    assert any("duplicate source id" in e for e in errors)


def test_bad_id_rejected():
    src = _good_source()
    src["id"] = "Bad ID!"
    assert any("id must match" in e for e in manifest.validate_register(_doc(src)))


def test_wrong_schema_version_rejected():
    doc = _doc(_good_source())
    doc["schema_version"] = 2
    assert any("schema_version" in e for e in manifest.validate_register(doc))


@pytest.mark.parametrize("bad", [None, [], "text", 7])
def test_non_object_source_rejected(bad):
    assert manifest.validate_register(_doc(bad))


def test_cli_exit_codes(tmp_path):
    good = tmp_path / "good.json"
    good.write_text(json.dumps(_doc(_good_source())), encoding="utf-8")
    assert manifest.main(["validate", "--release", str(good)]) == 0

    bad = tmp_path / "bad.json"
    doc = copy.deepcopy(_doc(_good_source()))
    doc["sources"][0]["status"] = "maybe"
    bad.write_text(json.dumps(doc), encoding="utf-8")
    assert manifest.main(["validate", str(bad)]) == 1

    assert manifest.main(["validate", str(tmp_path / "missing.json")]) == 2
