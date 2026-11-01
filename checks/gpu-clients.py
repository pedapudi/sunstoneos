#!/usr/bin/env python3
"""Compile actual GPU client API with modeled task, queue and service effects."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from harness import CLANG, SANITIZE, environment
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--output',type=Path,required=True)
p.add_argument('--baseline',default='9f7c1b00')
a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
(a.output/'FreeRTOS.h').write_text('''#ifndef HOST_FREERTOS_H
#define HOST_FREERTOS_H
#include <stdint.h>
#include <assert.h>
typedef uint32_t TickType_t; typedef int BaseType_t; typedef unsigned UBaseType_t;
typedef uint64_t StackType_t; typedef void *TaskHandle_t; typedef void (*TaskFunction_t)(void *);
typedef struct { unsigned unused; } StaticTask_t;
#define pdTRUE 1
#define pdFALSE 0
#define taskSCHEDULER_NOT_STARTED 0
#define taskSCHEDULER_RUNNING 1
#define configASSERT assert
#endif
''')
(a.output/'task.h').write_text('''#include "FreeRTOS.h"
int xTaskGetSchedulerState(void); TaskHandle_t xTaskGetCurrentTaskHandle(void);
TickType_t xTaskGetTickCount(void); void vTaskDelay(TickType_t);
''')
(a.output/'queue.h').write_text('''#ifndef HOST_QUEUE_H
#define HOST_QUEUE_H
#include "FreeRTOS.h"
typedef void *QueueHandle_t; typedef struct { unsigned unused; } StaticQueue_t;
QueueHandle_t xQueueCreateStatic(unsigned,unsigned,uint8_t *,StaticQueue_t *);
BaseType_t xQueueSend(QueueHandle_t,const void *,TickType_t);
BaseType_t xQueueReceive(QueueHandle_t,void *,TickType_t);
#endif
''')
flags=[str(CLANG),'-std=c11','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers',*SANITIZE,
       '-I'+str(a.output),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'apps/os'),'-include',str(a.output/'task.h')]
def build(source,name,baseline=False):
    target=a.output/name
    subprocess.run([*flags,*(['-DGPU_CLIENT_BASELINE=1'] if baseline else []),str(source),
        str(ROOT/'checks/gpu-clients.c'),str(ROOT/'apps/os/gpu-share.c'),'-o',str(target)],check=True,env=environment())
    return subprocess.run([str(target)],env=environment(),capture_output=True,text=True)
old=subprocess.check_output(['git','show',a.baseline+':drivers/gpu/gpu_queue.c'],cwd=ROOT)
(a.output/'baseline-client.c').write_bytes(old)
failed=build(a.output/'baseline-client.c','baseline',True)
assert failed.returncode!=0 and 'q.final_close' in failed.stderr,failed.stderr
fixed=build(ROOT/'drivers/gpu/gpu_queue.c','clients')
assert fixed.returncode==0,fixed.stdout+fixed.stderr
(a.output/'result.json').write_text(json.dumps({'passed':True,'baseline_revision':a.baseline,
    'baseline_sha256':hashlib.sha256(old).hexdigest(),'baseline_failure':failed.stderr.strip(),
    'client_sha256':hashlib.sha256((ROOT/'drivers/gpu/gpu_queue.c').read_bytes()).hexdigest(),
    'checks':fixed.stdout.strip(),'physical_access':False,
    'scope':'Actual client API and pure app admission, modeled scheduler/service/worker; no GPU execution or worker scheduling claim.'},indent=2)+'\n')
print(fixed.stdout,end='')
