/*
 * Keystone crypto accelerator driver
 *
 * Copyright (C) 2015,2016 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors:	Sandeep Nair
 *		Vitaly Andrianov
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#ifndef _KEYSTONE_SA_HLP_
#define _KEYSTONE_SA_HLP_

#include <linux/interrupt.h>
#include <linux/soc/ti/knav_dma.h>
#include <linux/regmap.h>
#include <asm/aes_glue.h>
#include <crypto/aes.h>

#define SA_RX_BUF0_SIZE 1500

#define SA_PID_OFS		0
#define SA_CMD_STATUS_OFS	0x8
#define SA_PA_FLOWID_OFS	0x10
#define SA_CDMA_FLOWID_OFS	0x14
#define	SA_PA_ENG_ID_OFS	0x18
#define	SA_CDMA_ENG_ID_OFS	0x1C

/* Crypto driver instance data */
struct keystone_crypto_data {
	struct platform_device	*pdev;
	struct clk		*clk;
	struct tasklet_struct	rx_task;
	struct tasklet_struct	tx_task;
	struct dma_pool		*sc_pool;
	struct kmem_cache	*dma_req_ctx_cache;
	struct regmap	*sa_regmap;

	void		*rx_chan;
	void		*rx_fdq[KNAV_DMA_FDQ_PER_CHAN];
	void		*rx_compl_q;
	void		*tx_chan;
	void		*tx_submit_q;
	void		*tx_compl_q;
	u32		tx_submit_qid;
	u32		tx_compl_qid;
	u32		rx_compl_qid;
	const char	*rx_chan_name;
	const char	*tx_chan_name;
	u32		tx_queue_depth;
	u32		rx_queue_depths[KNAV_DMA_FDQ_PER_CHAN];
	u32		rx_pool_size;
	u32		rx_pool_region_id;
	void		*rx_pool;
	u32		tx_pool_size;
	u32		tx_pool_region_id;
	void		*tx_pool;

	/* Security context data */
	u16		sc_id_start;
	u16		sc_id_end;
	u16		sc_id;
	atomic_t	rx_dma_page_cnt; /* N buf from 2nd pool available */
	atomic_t	tx_dma_desc_cnt; /* Tx DMA desc-s available */
};

/* Tx DMA callback param */
struct sa_dma_req_ctx {
	struct keystone_crypto_data *dev_data;
};

/* Encryption algorithms */
enum sa_alg_id {
	SA_EALG_ID_NONE = 0,	/* No encryption */
	SA_AALG_ID_NONE = SA_EALG_ID_NONE, /* No Authentication  */
	SA_EALG_ID_NULL,	/* NULL encryption */
	SA_EALG_ID_AES_CTR,	/* AES Counter mode */
	SA_EALG_ID_AES_F8,	/* AES F8 mode */
	SA_EALG_ID_AES_CBC,	/* AES CBC mode */
	SA_EALG_ID_DES_CBC,	/* DES CBC mode */
	SA_EALG_ID_3DES_CBC,	/* 3DES CBC mode */
	SA_EALG_ID_CCM,		/* Counter with CBC-MAC mode */
	SA_EALG_ID_GCM,		/* Galois Counter mode */
	SA_AALG_ID_NULL,	/* NULL Authentication  */
	SA_AALG_ID_MD5,		/* MD5 mode */
	SA_AALG_ID_SHA1,	/* SHA1 mode */
	SA_AALG_ID_SHA2_224,	/* 224-bit SHA2 mode */
	SA_AALG_ID_SHA2_256,	/* 256-bit SHA2 mode */
	SA_AALG_ID_HMAC_MD5,	/* HMAC with MD5 mode */
	SA_AALG_ID_HMAC_SHA1,	/* HMAC with SHA1 mode */
	SA_AALG_ID_HMAC_SHA2_224,	/* HMAC with 224-bit SHA2 mode */
	SA_AALG_ID_HMAC_SHA2_256,	/* HMAC with 256-bit SHA2 mode */
	SA_AALG_ID_GMAC,	/* Galois Message Auth. Code mode */
	SA_AALG_ID_CMAC,	/* Cipher-based Mes. Auth. Code mode */
	SA_AALG_ID_CBC_MAC,	/* Cipher Block Chaining */
	SA_AALG_ID_AES_XCBC,	/* AES Extended Cipher Block Chaining */
	SA_ALG_ID_LAST
};

/*
 * Mode control engine algorithms used to index the
 * mode control instruction tables
 */
enum sa_eng_algo_id {
	SA_ENG_ALGO_ECB = 0,
	SA_ENG_ALGO_CBC,
	SA_ENG_ALGO_CFB,
	SA_ENG_ALGO_OFB,
	SA_ENG_ALGO_CTR,
	SA_ENG_ALGO_F8,
	SA_ENG_ALGO_F8F9,
	SA_ENG_ALGO_GCM,
	SA_ENG_ALGO_GMAC,
	SA_ENG_ALGO_CCM,
	SA_ENG_ALGO_CMAC,
	SA_ENG_ALGO_CBCMAC,
	SA_NUM_ENG_ALGOS
};

/* 3DES only supports ECB, CBC, CFB and OFB. */
#define SA_3DES_FIRST_ALGO          SA_ENG_ALGO_ECB
#define SA_3DES_LAST_ALGO           SA_ENG_ALGO_OFB
#define SA_3DES_NUM_ALGOS           (SA_3DES_LAST_ALGO - SA_3DES_FIRST_ALGO + 1)

#define NKEY_SZ			3
#define MCI_SZ			27

struct sa_eng_info {
	u8	eng_id;
	u16	sc_size;
};

void sa_set_sc_auth(u16 alg_id, const u8 *key, u16 key_sz, u8 *sc_buf);
int sa_set_sc_enc(u16 alg_id, const u8 *key, u16 key_sz,
		  u16 aad_len, u8 enc, u8 *sc_buf);

void sa_swiz_128(u8 *in, u8 *out, u16 len);
void sa_conv_calg_to_salg(const char *cra_name, int *ealg_id, int *aalg_id);
struct sa_eng_info *sa_get_engine_info(int alg_id);
int sa_get_hash_size(u16 aalg_id);

/*
 * Derive sub-key k1, k2 and k3 used in the AES XCBC MAC mode
 * detailed in RFC 3566
 */
static inline int sa_aes_xcbc_subkey(u8 *sub_key1, u8 *sub_key2,
				     u8 *sub_key3, const u8 *key,
				     u16 key_sz)
{
	struct AES_KEY enc_key;

	if (private_AES_set_encrypt_key(key, (key_sz * 8), &enc_key)) {
		pr_err("%s: failed to set enc key\n", __func__);
		return -EINVAL;
	}

	if (sub_key1) {
		memset(sub_key1, 0x01, AES_BLOCK_SIZE);
		AES_encrypt(sub_key1, sub_key1, &enc_key);
	}

	if (sub_key2) {
		memset(sub_key2, 0x02, AES_BLOCK_SIZE);
		AES_encrypt(sub_key2, sub_key2, &enc_key);
	}

	if (sub_key3) {
		memset(sub_key3, 0x03, AES_BLOCK_SIZE);
		AES_encrypt(sub_key3, sub_key3, &enc_key);
	}

	return 0;
}

struct sa_eng_mci_tbl {
	uint8_t aes_enc[SA_NUM_ENG_ALGOS][NKEY_SZ][MCI_SZ];
	uint8_t aes_dec[SA_NUM_ENG_ALGOS][NKEY_SZ][MCI_SZ];
	uint8_t _3des_enc[SA_3DES_NUM_ALGOS][MCI_SZ];
	uint8_t _3des_dec[SA_3DES_NUM_ALGOS][MCI_SZ];
};

extern struct sa_eng_mci_tbl sa_mci_tbl;

extern struct device *sa_ks2_dev;

#endif /* _KEYSTONE_SA_HLP_ */
