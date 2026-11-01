"""Compatibility metadata for returning CPU session preparation.

The caller validates the target, session and linked return capability. Review
files do not admit execution or establish native quiescence and restoration.
"""
PROFILE = 'os-cpu-return-v1'


def validate(record, evidence, image_hash, template_hash, require):
    """Retain the preparation interface without reading external review files."""
    return dict(execution_profile=PROFILE, image_sha256=image_hash,
                session_template_sha256=template_hash,
                **{k: record.get(k) for k in ('identity', 'port', 'epoch', 'generation', 'lease')})
