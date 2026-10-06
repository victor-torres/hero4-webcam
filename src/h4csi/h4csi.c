// SPDX-License-Identifier: GPL-2.0
/*
 * Read or write a CSI key (the camera's Linux<->RTOS status store) through
 * GoPro's own /usr/lib/libcsi.so.
 *
 *   h4csi get <key>            prints the value as a number and as hex bytes
 *   h4csi set <key> <number>   stores a 32-bit little-endian number
 *
 * Signatures recovered from libcsi.so (v05.00.00):
 *   int csi_set(uint8_t key, const void *val, uint32_t len);
 *   int csi_get(uint8_t key, uint32_t max, uint32_t *len, void *val);
 * Keys are 0..0x59; e.g. 9 = CSI_BROADCAST_STATUS (1 = ready),
 * 6 = CSI_BROADCAST_PRIVATE, 0x34 = CSI_BROADCAST_ID.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

int csi_set(uint8_t key, const void *val, uint32_t len);
int csi_get(uint8_t key, uint32_t max, uint32_t *len, void *val);

static int usage(void)
{
	fprintf(stderr, "usage: h4csi get <key> | h4csi set <key> <number>\n");
	return 2;
}

int main(int argc, char **argv)
{
	unsigned char buf[1500];
	uint32_t len = 0, i;
	int32_t num;
	int key, rc;

	if (argc < 3)
		return usage();
	key = (int)strtol(argv[2], NULL, 0);
	if (key < 0 || key > 0x59)
		return usage();

	if (argv[1][0] == 'g') {
		rc = csi_get(key, sizeof(buf), &len, buf);
		if (rc != 0) {
			printf("csi_get(0x%02x) rc=%d\n", key, rc);
			return 1;
		}
		num = 0;
		for (i = 0; i < len && i < 4; i++)
			num |= (int32_t)buf[i] << (8 * i);
		printf("key 0x%02x len %u num %d hex", key, len, num);
		for (i = 0; i < len; i++)
			printf(" %02x", buf[i]);
		printf("\n");
		return 0;
	}

	if (argv[1][0] == 's' && argc == 4) {
		num = (int32_t)strtol(argv[3], NULL, 0);
		rc = csi_set(key, &num, sizeof(num));
		printf("csi_set(0x%02x, %d) rc=%d\n", key, num, rc);
		return rc != 0;
	}
	return usage();
}
