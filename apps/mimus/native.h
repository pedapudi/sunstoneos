#ifndef SUNSTONE_MIMUS_NATIVE_H
#define SUNSTONE_MIMUS_NATIVE_H
#ifdef __cplusplus
extern "C" {
#endif
void mimus_native_inference_progress(void);
void mimus_native_finished(int success);
int mimus_native_paced(void);
void mimus_native_waiting(int);
void mimus_native_input_progress(void);
void mimus_application_start(void);
#ifdef __cplusplus
}
#endif
#endif
