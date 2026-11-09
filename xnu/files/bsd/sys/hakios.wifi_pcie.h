#ifndef _SYS_LEMONADE_WIFI_H_
#define _SYS_LEMONADE_WIFI_H_
#include <stdbool.h>
#include <stdint.h>

/* Available only while the exact-platform PCIe/MHI runtime owns the device. */
void hakiospcie_wifi_stop(void);
/* Read a QMI memory request wherever the transport hands it over, before ath11k decodes it. */
void hakiospcie_note_memory_request(const uint8_t *data, unsigned size);
unsigned hakiospcie_wifi_driver_mode(void);
void *hakiospcie_wifi_driver_memory(unsigned long size, unsigned long long *iova);
int hakiospcie_wifi_regdb_skipped(const char *bare_name);
const uint8_t *hakiospcie_wifi_driver_file(const char *name, uint64_t *size);
void *hakiospcie_wifi_driver_bar(void);
uint32_t hakiospcie_wifi_register_read(uint32_t offset);
void hakiospcie_wifi_register_write(uint32_t offset, uint32_t value);
#endif
