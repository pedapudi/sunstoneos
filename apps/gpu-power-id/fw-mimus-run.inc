/* Two bounded exact-sum jobs on the already published compute context. */
static int mimus_run(uint64_t wait_end, uint64_t *first_kick, uint64_t *last_done)
{
    pvr_mimus_session_command(&mimus_session, mimus_command, 0, job_program_result.cdm_resume_address);
    job_outcome = 4;
    for (unsigned sequence = 1; sequence <= FWS_JOBS; ++sequence) {
        const unsigned k = sequence == 1 ? 512 : 516;
        struct pvr_mimus_tensor tensor;
        uint32_t previous;
        job.sequence = sequence;
        job.phase = PVR_JOB_NEW;
        if (bb_power_now() + JOB_REPEAT_RESERVE_US >= wait_end ||
            !job_read_fence(NULL, &previous) || previous != sequence - 1 ||
            !mimus_inputs(k, sequence) ||
            !pvr_mimus_tensor_open(&tensor, 0, 0, k, k, MIMUS_CHANNELS)) goto failed;
        const struct pvr_mimus_job request = {0, MIMUS_ROWS, 2 * k, sequence};
        if (!pvr_mimus_submit(&mimus_session, &tensor, &request)) goto failed;
        mimus_update[0] = job_object_address[5]; mimus_update[1] = sequence;
        const struct pvr_cccb_command commands[2] = {
            {PVR_CCB_MAGIC | PVR_CCCB_TASK | 205u, 96, sequence, sequence, mimus_command},
            {PVR_CCB_MAGIC | 213u, 8, 0, 0, mimus_update}
        };
        if (!pvr_cccb_begin(&job_client, commands, 2, NULL, 0, bb_power.deadline)) goto failed;
        job.serial = job_client.serial;
        job.phase = PVR_JOB_KICK;
        for (;;) {
            if (pvr_gpu_control_poll(&control, 4) == PVR_GPU_CONTROL_FAULT) goto failed;
            if (job_resets || job_unexpected) { job_outcome = 5; goto failed; }
            if (bb_power_now() >= wait_end) { job_outcome = 2; goto failed; }
            const TickType_t begun = xTaskGetTickCount();
            const enum pvr_cccb_result r = pvr_cccb_poll(&job_client);
            if (r != PVR_CCCB_AGAIN && r != PVR_CCCB_OK) goto failed;
            if (job_client.phase == PVR_CCCB_WAIT && !job_runs[sequence - 1].passes) {
                if (!*first_kick) *first_kick = fws_mts_tick;
                const uint64_t now = bb_power_now();
                const uint64_t end = fws_counter() + (wait_end > now ? wait_end - now : 0) * JOB_TICKS_PER_US;
                if (!job_spin(sequence, end, begun)) { job_outcome = 2; goto failed; }
                *last_done = fws_mts_tick + job_runs[sequence - 1].ticks;
            }
            if (r == PVR_CCCB_OK) break;
        }
        job.phase = PVR_JOB_WAIT_FENCE;
        if (!job_read_fence(NULL, &previous) || previous != sequence) { job_outcome = 2; goto failed; }
        if (!mimus_verify(k, sequence)) { job_outcome = 3; goto failed; }
        if (bb_power_now() >= wait_end) { job_outcome = 2; goto failed; }
        job.phase = PVR_JOB_COMPLETE;
        ++job_jobs;
    }
    job_outcome = 1;
failed:
    if (job_outcome != 1) pvr_mimus_stop(&mimus_session);
    struct bb_line line;
    bb_line_start(&line, "GPU exact sums");
    bb_line_dec(&line, "jobs", job_jobs); bb_line_dec(&line, "sums", mimus_verified);
    bb_line_dec(&line, "bad_tags", mimus_bad_tags); bb_line_dec(&line, "bad_sums", mimus_bad_sums);
    bb_line_dec(&line, "bad_guards", mimus_bad_guards);
    bb_line_hex(&line, "minimum", (uint64_t)mimus_minimum); bb_line_hex(&line, "maximum", (uint64_t)mimus_maximum);
    bb_line_end(&line); bb_power_print(&line);
    return job_outcome == 1 ? 1 : -1;
}
