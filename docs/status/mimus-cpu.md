# CPU recognizer landing qualification

The CPU fixes are on work/cpu-perf-os-1031 from adb6ca03. Writable C++
constructor/GOT metadata occupies image RAM. The clock record publishes the
applied DSU point. The external application source record selects the final
mimus revision on port/sunstoneos-split through MIMUS_DIR.

Independent review finds no blocking defect. Three baseline builds and one
selected integration gate pass, including the check index and eight-core
scheduler model. Focused native checks pass 28 owner and nine adapter cases.
The application runtime trees match the validated revision a0d2c48a5; the final
application commit adds documentation only. Evidence is under
~/.cache/sunstoneos/build/cpu-perf-1031/landing-validation/.

Physical corpus qualification is partial. Five batches return normally with
exact reference output for 13 recordings at CPU-L 1794 MHz and 1000 mV,
CPU-B 2002 MHz and 1050 mV, and DSU 1274 MHz. The 8.87-second clip takes
388121 microseconds; the 29.84-second clip takes 2568352 microseconds.
Five clips exceed the 0.05 real-time-factor target. The 3.59-second reference
meets its 179.5 ms limit.

Port 3-1 is paused because its queue daemon was interrupted during a run.
Nineteen recordings remain unsubmitted in batches 3, 5, 6, 7 and 8. No further
board command is authorized until coordinator inspection and queue resumption.
The completed hypotheses must not repeat. The landing image SHA-256 is
052b43f6b96c969d86619b2b3694e8ee5231c9e6bc4b7ea8e5ea2f2a24b8d019.
The plan, partial result and executed commands are in the task's corpus-landing/
evidence directory. Attention matrix products, linear GEMM/scaling and
quantization/producers are the next measured CPU candidates; none is changed.
