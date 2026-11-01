The NPU application API in `sunstone_app.h` submits one request through a
memory mailbox. The privileged OS task owns the runtime slot and copies the
concatenated input and output tensors. Applications retain their buffers until
completion. A failed request permanently retains those buffers for the session.

`ss_npu_model_count` reports the linked catalogue size. `ss_npu_model_find`
resolves a semantic name to an image-local index; `ss_npu_model_info` returns
that name, tensor counts and concatenated byte sizes. Names occupy at most
63 UTF-8 bytes plus a terminating zero. The backend checks this immutable
metadata against decoded model tensors before publishing the catalogue.
`ss_npu_run` refuses an unknown index or mismatched buffer extent before posting
a request. It selects an image-linked model index. Call with identical arguments
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

Build the finite diagnostic with `apps/npu-finite/build.py --sdk --evidence`
followed by a new build directory. It links `apps/npu-sdk/finite-client.c`
and `runtime/npu_app.c`. The service task owns the job helper;
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

The OS catalogue generator accepts `--models <application model-list JSON>`
and `--model-name <catalogue name>`. The default links only the selected model,
whose local index is zero. `--all` links the entire list in data order. The
source-list index in the diagnostic report and the local SDK index have separate
symbols. The OS runtime admits at most sixteen resident entries; that capacity
bounds storage and does not require a particular catalogue count or ordering.
Model names and tensor geometry come from the reviewed catalogue, and every
linked model and selected vector retains its size and SHA-256 checks.

`catalogue-check.py --artifacts <map> --native-flags <OS flags.make> --output
<new cache directory>` generates catalogues with one, two and three entries.
It decodes each actual linked model, exercises the OS metadata bind and resolves
the published ShuffleNet by name in different positions. Wrong decoded counts,
wrong decoded sizes, unknown indices and incorrect request extents must be
refused. The selected-only variants also check that the local index remains zero.
No device is opened and no board image is linked.

`metadata-check.c` compiles with `runtime/npu_app.c` and `-Iinclude`. Run it with
each argument from `0` through `10` to check malformed metadata refusal before
binding. Its backend aborts if a refused case reaches a device operation.
