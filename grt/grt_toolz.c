#include <inttypes.h>
#include "grt_toolz.h"

void grt_printHexArray(char* array, uint16_t array_length) {
	printf("[");
	int i;
	for (i=0;i < array_length;i++) {
    		if (i == array_length-1) printf("%02X", (uint8_t)array[i]);
		else printf("%02X ", (uint8_t)array[i]);
		
	}
	printf("]");
}

void grt_printIntArray(uint8_t* array, uint8_t array_length) {
	printf("[");
	int i;
	for (i=0;i < array_length;i++) {
    		if (i == array_length-1) printf("%i",array[i]);
		else printf("%i,",(uint8_t)array[i]);
		
	}
	printf("]");
}

int grt_ethaddr_to_string(char* strtarget, const struct ether_addr* addr) {

	return sprintf (strtarget, "%02x:%02x:%02x:%02x:%02x:%02x", 
		addr->addr_bytes[0],
		addr->addr_bytes[1],
		addr->addr_bytes[2],
		addr->addr_bytes[3],
		addr->addr_bytes[4],
		addr->addr_bytes[5]);
}

int grt_ethertype_to_string(char* str2write, const uint16_t ether_type) {
	return sprintf (str2write, "0x%02x%02x", ether_type & 0x00ff, ether_type >> 8);
}


void print_ipv4(const uint32_t ip_addr) {

	printf("%i.%i.%i.%i", 
		(ip_addr >> 24) & 0xff,
		(ip_addr >> 16) & 0xff,
		(ip_addr >> 8) & 0xff,
		ip_addr & 0xff);

}

int grt_ipv4_to_string(char* str2write, const uint32_t ip_addr) {
	
	return sprintf(str2write, "%i.%i.%i.%i", 
		(ip_addr >> 24) & 0xff,
		(ip_addr >> 16) & 0xff,
		(ip_addr >> 8) & 0xff,
		ip_addr & 0xff);


}











