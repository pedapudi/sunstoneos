#ifndef SUNSTONE_SPI_IRQ_H
#define SUNSTONE_SPI_IRQ_H
/* One fixed CPU0 Group1NS level source. The EL3 route and dispatcher belong
 * to the selected application startup, and ACK/EOI remain in the port. */
int spi_irq_bind(void);
void spi_interrupt(void);
int spi_irq_arm(void);
int spi_irq_claim(void);
void spi_irq_stop(void);
void spi_irq_wait(void);
unsigned spi_irq_notifications(void);
#endif
