"""Exact source-review obligations for the staged CPU normal-return profile."""
import json

PROFILE = 'os-cpu-return-v1'
CONDITIONS = {'architecture-reset-reviewed', 'cpu-and-service-quiescence-reviewed',
              'irq-cache-and-memory-retirement-reviewed', 'cpu-clock-and-rail-handoff-reviewed',
              'failure-retention-reviewed'}


def validate(record, evidence, image_hash, template_hash, require):
    require(isinstance(record.get('normal_return_review'), dict),
            'Exact CPU normal-return quiescence/source review required.')
    review = json.loads(evidence(record['normal_return_review']))
    require(review.get('schema') == 'a733-os-cpu-return-review-v1' and
            review.get('execution_profile') == PROFILE and review.get('reviewed') is True and
            review.get('image_sha256') == image_hash and
            review.get('session_template_sha256') == template_hash and
            all(review.get(k) == record.get(k) for k in ('identity', 'port', 'epoch', 'generation', 'lease')) and
            isinstance(review.get('supervisor'), str) and review['supervisor'].strip() and
            review.get('failure_policy') == 'retain-without-retry' and
            review.get('rail_followup') == 'verify-boot-clocks-and-restore-cpu-rails' and
            set(review.get('conditions', {})) == CONDITIONS,
            'Exact CPU normal-return quiescence/source review required.')
    require(record.get('whole_board_power_removal_on_failure') is True and
            record.get('no_resource_reuse_before_power_removal_on_failure') is True,
            'CPU return failure retains the power-removal/no-reuse obligation.')
    require('whole_board_power_removal' not in record and 'no_resource_reuse_before_power_removal' not in record,
            'CPU return requires explicit failure-only retention facts.')
    for item in review['conditions'].values():
        evidence(item)
    require(review.get('sources'), 'CPU return source evidence is absent.')
    for item in review['sources']:
        evidence(item)
    return review
