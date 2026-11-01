The NPU application API in `sunstone_app.h` submits one request through a
memory mailbox. The privileged OS task owns the runtime slot and copies the
concatenated input and output tensors. Applications retain their buffers until
completion. A failed request permanently retains those buffers for the session.

`ss_npu_run` selects an image-linked model index. Call with identical arguments
until it returns a nonzero result. Zero means pending, one means copied output,
minus two means refused, and minus one means unavailable or failed. The API
accepts at most one MiB in each direction and a SUBMIT timeout up to one second.
`ss_npu_close` ends the slot after the final completed request. In the finite
diagnostic, its privileged service task also waits for local stop and parks
the providers before publishing completion. The application observes the
resulting quiescent state.

The OS NPU service uses this mailbox when the image has an application manifest.
Images without an application retain the built-in catalogue diagnostic. The
application must close the NPU before finishing. Thermal STOP also closes the
slot and makes the application mailbox unavailable.

For the finite diagnostic, replace `apps/npu-finite/runtime-client.c` with
`apps/npu-sdk/finite-client.c` and add `runtime/npu_app.c`. Keep the finite
runtime include paths and definitions. The service task owns the job helper;
the client calls the public API for both linked models and compares copied
outputs. The existing guarded close, local stop and quiescence checks remain.
This diagnostic runs the client at the finite image's task privilege; the API
implementation itself uses ordinary memory and atomics suitable for EL0.

Run the mailbox host test with a C11 compiler and `-Iinclude`, linking
`runtime/npu_app.c` and `apps/npu-sdk/host-check.c`. Run the executable once
without arguments, with `failure`, and with `refused`. These cases cover request ownership,
copied output, refused work, terminal close and permanent failure retention. They do not
qualify physical NPU operation or OS application mappings.

`apps/npu-sdk/interleave-check.c` compiles alone with `-Iinclude`. Run its
executable with each argument from `0` through `3`. It injects service failure
between the application's state observation and publication for each request
and completion transition. Every case must retain the failed state.

`owner-check.py --image <SDK ELF> --out <new cache directory>` executes the
linked provider-quiesce authorization gate. It requires the application task
to be refused and the SDK close service owner to pass. Provider operations
after authorization are modeled.

`client-check.py` accepts the same arguments. It executes the linked public
mailbox API and service-step function, pumping the service when the application
waits. Backend job replies, startup publications and provider parking are
modeled. The cases cover successful inference, final-output mismatch, missing
IRQ completion, failed backend job and failed close.
