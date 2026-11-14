/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef SS_KERNEL_PERTURB_H
#define SS_KERNEL_PERTURB_H
/* Host models only: a delay of loaded loop turns at named scheduler sites, to
 * shift every later interleaving on QEMU's deterministic clock. Word 0 of the
 * table at address SS_KERNEL_TEST_PERTURB selects the cores that delay (zero
 * means all); word 1 + site holds the turns for that site. QEMU loads the
 * table; production images define no table and compile no hook. */
enum ss_kernel_perturb_site {
    SS_PERTURB_API_ENTRY,      /* kernel API entry, before any lock or mask */
    SS_PERTURB_OWNER_PUBLISH,  /* publisher: hint visible, doorbell decision not yet made */
    SS_PERTURB_OWNER_DRAIN,    /* owner: hints detached, not yet inspected */
    SS_PERTURB_CONTROL_OPEN,   /* owner: control batch admitted with interrupts open */
    SS_PERTURB_OBJECT_WALK,    /* waker: a waiter-list item and its successor read, neither yet used */
    SS_PERTURB_SITES
};
#ifdef SS_KERNEL_TEST_PERTURB
void ss_kernel_perturb(enum ss_kernel_perturb_site site);
#define SS_KERNEL_PERTURB(site) ss_kernel_perturb(site)
#else
#define SS_KERNEL_PERTURB(site) ((void)0)
#endif
#endif
