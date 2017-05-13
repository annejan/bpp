/*
Packet signature checking

Every packet sent out is signed using ECDSA.
*/
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <arpa/inet.h>
#include "recvif.h"
#include "structs.h"
#include "mbedtls/config.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdsa.h"

#include "../keys/pubkey.inc"
#include "mbedtls/sha256.h"

static RecvCb *recvCb;

//We only use the key as a handy store for Q and grp.
static mbedtls_ecp_keypair key;


#define ECPARAMS MBEDTLS_ECP_DP_SECP256R1


void chksignInit(RecvCb *cb) {
	int r;
	recvCb=cb;

	r=mbedtls_ecp_group_load(&key.grp, ECPARAMS);
	if (r) printf("group load failed\n");
	r=mbedtls_mpi_read_binary(&key.Q.X, (char*)&public_key[0], 32);
	if (r) printf("read_binary X failed\n");
	r=mbedtls_mpi_read_binary(&key.Q.Y, (char*)&public_key[32], 32);
	if (r) printf("read_binary Y failed\n");
	r=mbedtls_mpi_read_binary(&key.Q.Z, "\001", 1);
	if (r) printf("read_binary Z failed\n");
}

void chksignRecv(uint8_t *packet, size_t len) {
	if (len<sizeof(SignedPacket)) return;
	SignedPacket *p=(SignedPacket*)packet;
	int plLen=len-sizeof(SignedPacket);

	uint8_t hash[32];
	mbedtls_sha256(p->data, plLen, hash, 0);
	
	mbedtls_mpi mpir, mpis;
	mbedtls_mpi_init(&mpir);
	mbedtls_mpi_init(&mpis);
	mbedtls_mpi_read_binary(&mpir, (char*)&p->sig[0], 32);
	mbedtls_mpi_read_binary(&mpis, (char*)&p->sig[32], 32);
	int isOk=!mbedtls_ecdsa_verify(&key.grp, hash, sizeof(hash), &key.Q, &mpir, &mpis);

	if (isOk) {
		recvCb(p->data, plLen);
	} else {
		printf("Huh? ECDSA signature mismatch!\n");
	}
}
