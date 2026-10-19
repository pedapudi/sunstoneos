/* Focused changed FIFO ownership rule; actual controller implementation. */
#include "../drivers/dma/a733_dma_controller.c"
#include <assert.h>
#include <stdio.h>
int main(void)
{
 struct a733_dma_segment rx={{0x02502000,16,1,1},{0x41800000,0,1,1},257};
 struct a733_dma_segment tx={{0x41801000,0,1,1},{0x02502000,16,1,1},257};
 struct a733_dma_mapping rm={(void *)0x41800000,0x41800000,4096,A733_DMA_WRITE};
 struct a733_dma_mapping tm={(void *)0x41801000,0x41801000,4096,A733_DMA_READ};
 struct a733_dma_transfer r={{(void *)0x417fe000,0x417fe000,4096,A733_DMA_DESCRIPTORS},&rx,1,&rm,1};
 struct a733_dma_transfer t={{(void *)0x417ff000,0x417ff000,4096,A733_DMA_DESCRIPTORS},&tx,1,&tm,1};
 assert(!fifo_conflict(&r,&t)&&!fifo_conflict(&t,&r));
 tx.source=rx.source;tx.destination.address=0x41801000;tx.destination.request=0;
 assert(fifo_conflict(&r,&t)&&fifo_conflict(&t,&r));
 tx=(struct a733_dma_segment){{0x41801000,0,1,1},{0x02502000,16,1,1},257};
 rx=tx;rx.source.address=0x41800000;assert(fifo_conflict(&r,&t));
 rx=(struct a733_dma_segment){{0x02502000,16,1,1},{0x41800000,0,1,1},257};
 tm.bus=0x02502000;assert(fifo_conflict(&r,&t));
 tm.bus=0x41801000;t.descriptors.bus=0x02502000;assert(fifo_conflict(&r,&t));
 puts("PASS opposite-direction FIFO sharing; same-direction, FIFO/RAM and descriptor conflicts retained");
}
