#include <rte_config.h>
#include <rte_common.h>
#include <rte_ether.h>

extern void grt_printIntArray(uint8_t*, uint8_t);

extern int grt_ethaddr_to_string(char*, const struct ether_addr*);

extern int grt_ethertype_to_string(char*, const uint16_t);

extern void print_ipv4(const uint32_t);

extern int grt_ipv4_to_string(char*, const uint32_t);

void grt_printHexArray(char* array, uint16_t array_length);

