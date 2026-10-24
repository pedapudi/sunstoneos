/* Address-width limits, IOMMU leaf cost across 4 GiB, the master IOVA policy
 * and the DRAM below 4 GiB that width rules require. Host model only. */
#include "dma_address_width.h"
#include "a733_camera_native.h"
#include "a733_iommu.h"
#include "a733_pcie_epf_test.h"
#include "a733_pcie_nvme.h"
#include "a733_xhci.h"
#include <assert.h>
#include <stdio.h>

_Static_assert(A733_DMA0_DATA_LIMIT == UINT64_C(0x800000000), "DMA0 data width");
_Static_assert(A733_DMA0_DESCRIPTOR_LIMIT == UINT64_C(0xfffff800), "DMA0 end-of-chain marker");
_Static_assert(A733_IOMMU_PHYSICAL_LIMIT == UINT64_C(0x400000000), "IOMMU entry bits 9:8");
_Static_assert(A733_IOMMU_IOVA_LIMIT == UINT64_C(0x100000000), "IOMMU master IOVA policy");
_Static_assert(DMA_ADDRESS_32BIT_LIMIT == UINT64_C(0x100000000), "32-bit masters");

static _Alignas(16384) uint32_t arena[A733_IOMMU_TABLE_BYTES / 4];

static unsigned leaf_words(uint64_t physical, uint64_t bytes)
{
    const struct a733_iommu_span span = {0x10000000, physical, bytes, A733_IOMMU_WRITE};
    struct a733_iommu_tables tables;
    assert(a733_iommu_build(&tables, arena, 0x48000000, &span, 1) == 1);
    unsigned count = 0;
    for (unsigned i = 4096; i < A733_IOMMU_TABLE_BYTES / 4; ++i) count += arena[i] != 0;
    return count;
}

static int stub_now(void *c, uint64_t *v) { (void)c; *v = 0; return 1; }
static int stub_admit(void *c, const struct a733_iommu_tables *t, uint64_t d,
                      struct a733_iommu_admission *a)
{ (void)c; (void)t; (void)d; (void)a; assert(!"admission must not run"); return -1; }
static int stub_read(void *c, uint64_t k, uint32_t o, uint32_t *v, uint64_t d)
{ (void)c; (void)k; (void)o; (void)v; (void)d; assert(!"no register read"); return -1; }
static int stub_write(void *c, uint64_t k, uint32_t o, uint32_t v, uint64_t d)
{ (void)c; (void)k; (void)o; (void)v; (void)d; assert(!"no register write"); return -1; }
static int stub_publish(void *c, uint64_t k, const void *p, size_t n, uint64_t d)
{ (void)c; (void)k; (void)p; (void)n; (void)d; assert(!"no publication"); return -1; }

/* Initialization admits a span that ends exactly at the IOVA policy limit and
 * refuses one page beyond it, although both lie inside the 34-bit IOVA space. */
static void iova_policy(void)
{
    static const struct a733_iommu_ops ops = {stub_now, stub_admit, stub_read, stub_write, stub_publish};
    const uint64_t ends[] = {A733_IOMMU_IOVA_LIMIT, A733_IOMMU_IOVA_LIMIT + 4096};
    for (unsigned k = 0; k < 2; ++k) {
        const struct a733_iommu_span span = {ends[k] - 8192, UINT64_C(0x180000000), 8192, A733_IOMMU_READ};
        struct a733_iommu_tables tables;
        assert(a733_iommu_build(&tables, arena, 0x48000000, &span, 1) == 1);
        static struct a733_iommu state;
        state = (struct a733_iommu){0};
        assert(a733_iommu_init(&state, &ops, NULL, &tables) == (k ? -1 : 1));
        assert(state.initialized == !k);
    }
}

int main(void)
{
    /* One 640x480 two-byte plane; the first starts one page below 4 GiB. */
    const unsigned low = leaf_words(UINT64_C(0xfffff000), 307200);
    const unsigned high = leaf_words(UINT64_C(0x180000000), 307200);
    assert(low == 75 && high == 75);
    iova_policy();

    /* The native camera gives the IOMMU fixed device addresses, so its
     * allocation may lie anywhere below 2^34 and is not counted. */
    const unsigned long xhci = A733_XHCI_MEMORY_BYTES;
    const unsigned long nvme = A733_PCIE_NVME_POOL_BYTES;
    const unsigned long endpoint = 2ul * A733_PCIE_TEST_ALLOCATION + A733_PCIE_TEST_BYTES;
    const unsigned long total = xhci + nvme + endpoint;
    assert(nvme == 24576ul && total == 184320ul);
    assert(A733_CAMERA_NATIVE_IOVA + A733_CAMERA_NATIVE_BYTES <= A733_IOMMU_IOVA_LIMIT);
    printf("PASS DMA address width: limits, %u leaf words per plane below and above 4 GiB, "
           "IOVA policy at 4 GiB, %lu bytes below 4 GiB required "
           "(xHCI %lu, NVMe %lu, endpoint test %lu) "
           "plus one descriptor page per DMA0 job table. Hardware untested.\n",
           low, total, xhci, nvme, endpoint);
}
