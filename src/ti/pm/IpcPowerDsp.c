/*
 * Copyright (c) 2012, Texas Instruments Incorporated
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * *  Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *
 * *  Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * *  Neither the name of Texas Instruments Incorporated nor the names of
 *    its contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
 * OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
 * OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
 * EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
/** ============================================================================
 *  @file       IpcPowerDsp.c
 *
 *  @brief      A simple Power Managment which responses to the host commands.
 *
 *  TODO:
 *     - Add suspend/resume notifications
 *  ============================================================================
 */

#include <xdc/std.h>
#include <xdc/runtime/System.h>
#include <xdc/runtime/Error.h>
#include <xdc/runtime/Assert.h>
#include <xdc/runtime/Memory.h>
#include <xdc/runtime/Main.h>
#include <xdc/runtime/Log.h>
#include <xdc/runtime/Diags.h>

#include <ti/sysbios/hal/Hwi.h>
#include <ti/sysbios/knl/Swi.h>
#include <ti/sysbios/family/c64p/tesla/Power.h>
#include <ti/sysbios/family/c64p/tesla/Wugen.h>
#include <ti/sysbios/family/c64p/tesla/package/internal/Power.xdc.h>

#include <ti/ipc/MultiProc.h>
#include <ti/pm/IpcPower.h>
#include "_IpcPower.h"

#define REG32(A)   (*(volatile UInt32 *) (A))

static UInt32 IpcPower_hibLock;

#define PDCCMD_REG      0x01810000
#define SLEEP_MODE      0x15555
#define GPT5_IRQ        51
#define GPT6_IRQ        52
#define MBX_DSP_IRQ     55

static Swi_Handle suspendResumeSwi;

/* List for storing all registered callback functions */
static IpcPower_CallbackElem *IpcPower_callbackList = NULL;

/* Module ref count: */
static Int curInit = 0;


/*
 *  ======== IpcPower_callUserFxns ========
 */
static Void IpcPower_callUserFxns(IpcPower_Event event)
{
    IpcPower_CallbackElem *node = IpcPower_callbackList;

    /* Call the registered functions matching the event */
    while (node != NULL) {
        if (node->event == event) {
            (*(node->callback))(event, node->data);
        }
        node = node->next;
    }
}

/*
 *  ======== IpcPower_suspendSwi ========
 */
static Void IpcPower_suspendSwi(UArg arg0, UArg arg1)
{
    /* Disable wakeup events */
    Wugen_disableEvent(GPT5_IRQ);
    Wugen_disableEvent(GPT6_IRQ);
    Wugen_disableEvent(MBX_DSP_IRQ);

    /* Call pre-suspend preparation function */
    IpcPower_preSuspend();

    /* Invoke the BIOS suspend routine */
    Power_suspend(Power_Suspend_HIBERNATE);

    /* Re-enable wakeup events */
    Wugen_enableEvent(GPT5_IRQ);
    Wugen_enableEvent(GPT6_IRQ);
    Wugen_enableEvent(MBX_DSP_IRQ);

    /* Call post-resume preparation function */
    IpcPower_postResume();
}

/*
 * =============================================================================
 *  IpcPower Functions:
 * =============================================================================
 */

/*
 *  ======== IpcPower_init ========
 */
Void IpcPower_init()
{
    Swi_Params swiParams;

    if (curInit++) {
        return;
    }

    IpcPower_hibLock = 0;

    Swi_Params_init(&swiParams);
    swiParams.priority = Swi_numPriorities - 1; /* Max Priority Swi */
    suspendResumeSwi = Swi_create(IpcPower_suspendSwi, &swiParams, NULL);
}

/*
 *  ======== IpcPower_exit ========
 */
Void IpcPower_exit()
{
    --curInit;
}

/*
 *  ======== IpcPower_suspend ========
 */
Void IpcPower_suspend()
{
    Assert_isTrue((curInit > 0) , NULL);

    Swi_post(suspendResumeSwi);
}

/*
 *  ======== IpcPower_idle ========
 */
Void IpcPower_idle()
{
    REG32(PDCCMD_REG) = SLEEP_MODE;
    REG32(PDCCMD_REG);

    /* Enable wakeup events */
    Wugen_enableEvent(GPT5_IRQ);
    Wugen_enableEvent(GPT6_IRQ);
    Wugen_enableEvent(MBX_DSP_IRQ);

    asm(" idle");
}

/*
 *  ======== IpcPower_hibernateLock ========
 */
UInt IpcPower_hibernateLock()
{
    IArg hwiKey, swiKey;

    hwiKey = Hwi_disable();
    swiKey = Swi_disable();

    IpcPower_hibLock++;

    Swi_restore(swiKey);
    Hwi_restore(hwiKey);

    return (IpcPower_hibLock);
}

/*
 *  ======== IpcPower_hibernateUnlock ========
 */
UInt IpcPower_hibernateUnlock()
{
    IArg hwiKey, swiKey;

    hwiKey = Hwi_disable();
    swiKey = Swi_disable();

    if (IpcPower_hibLock > 0) {
        IpcPower_hibLock--;
    }

    Swi_restore(swiKey);
    Hwi_restore(hwiKey);

    return (IpcPower_hibLock);
}


/*
 *  ======== IpcPower_canHibernate ========
 */
Bool IpcPower_canHibernate()
{
    if (IpcPower_hibLock) {
        return (FALSE);
    }

    return (TRUE);
}

/*
 *  ======== IpcPower_registerCallback ========
 */
#define FXNN "IpcPower_registerCallback"
Int IpcPower_registerCallback(Int event, IpcPower_CallbackFuncPtr cbck,
                              Ptr data)
{
    IArg hwiKey;
    IpcPower_CallbackElem **list, *node;
    BIOS_ThreadType context = BIOS_getThreadType();

    if ((context != BIOS_ThreadType_Task) &&
        (context != BIOS_ThreadType_Main)) {
        Log_print0(Diags_ERROR, FXNN":Invalid context\n");
        return (IpcPower_E_FAIL);
    }

    list = &IpcPower_callbackList;

    /* Allocate and update new element */
    node = Memory_alloc(NULL, sizeof(IpcPower_CallbackElem), 0, NULL);
    if (node == NULL) {
        Log_print0(Diags_ERROR, FXNN":out of memory\n");
        return (IpcPower_E_MEMORY);
    }

    node->next     = NULL;
    node->event    = (IpcPower_Event) event;
    node->callback = cbck;
    node->data     = data;

    hwiKey = Hwi_disable();  /* begin: critical section */
    while (*list != NULL) {
        list = &(*list)->next;
    }
    *list = node;
    Hwi_restore(hwiKey);  /* end: critical section */

    return (IpcPower_S_SUCCESS);
}
#undef FXNN

/*
 *  ======== IpcPower_unregisterCallback ========
 */
#define FXNN "IpcPower_unregisterCallback"
Int IpcPower_unregisterCallback(Int event, IpcPower_CallbackFuncPtr cbck)
{
    IArg hwiKey;
    IpcPower_CallbackElem **list, *node;
    Int status = IpcPower_E_FAIL;
    BIOS_ThreadType context = BIOS_getThreadType();

    if ((context != BIOS_ThreadType_Task) &&
        (context != BIOS_ThreadType_Main)) {
        Log_print0(Diags_ERROR, FXNN":Invalid context\n");
        return (status);
    }

    list = &IpcPower_callbackList;
    node  = NULL;

    hwiKey = Hwi_disable();  /* begin: critical section */
    while (*list != NULL) {
        if ( ((*list)->callback == cbck) &&
             ((*list)->event == event) ) {
            node   = *list;
            *list  = (*list)->next;
            status = IpcPower_S_SUCCESS;
            break;
        }
        list = &(*list)->next;
    }
    Hwi_restore(hwiKey);  /* end: critical section */

    if (status == IpcPower_S_SUCCESS) {
        if (node != NULL) {
            Memory_free(NULL, node, sizeof(IpcPower_CallbackElem));
        }
        else {
            Log_print0(Diags_ERROR, FXNN":Invalid pointer\n");
        }
    }

    return (status);
}
#undef FXNN

/*
 *  ======== IpcPower_preSuspend ========
 */
Void IpcPower_preSuspend(Void)
{
    /* Call all user registered suspend callback functions */
    IpcPower_callUserFxns(IpcPower_Event_SUSPEND);
}

/*
 *  ======== IpcPower_postResume ========
 */
Void IpcPower_postResume(Void)
{
    /* Call all user registered resume callback functions */
    IpcPower_callUserFxns(IpcPower_Event_RESUME);
}
