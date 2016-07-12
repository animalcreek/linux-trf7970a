/*
 * Keystone crypto accelerator driver
 *
 * Copyright (C) 2015, 2016 Texas Instruments Incorporated - http://www.ti.com
 *
 * Authors:	Sandeep Nair
 *		Vitaly Andrianov
 *
 * Contributors:Tinku Mannan
 *		Hao Zhang
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

#include <linux/soc/ti/knav_dma.h>
#include <linux/soc/ti/knav_qmss.h>

#include "keystone-sa.h"
#include "keystone-sa-hlp.h"

#define SA_CMDL_UPD_ENC		0x0001
#define SA_CMDL_UPD_AUTH	0x0002
#define SA_CMDL_UPD_ENC_IV	0x0004
#define SA_CMDL_UPD_AUTH_IV	0x0008
#define SA_CMDL_UPD_AUX_KEY	0x0010

/* Command Label Definitions and utility functions */
struct sa_cmdl_cfg {
	int	enc1st;
	int	aalg;
	u8	enc_eng_id;
	u8	auth_eng_id;
	u8	iv_size;
	const u8 *akey;
	u16	akey_len;
};

/* Format general command label */
static int sa_format_cmdl_gen(struct sa_cmdl_cfg *cfg, u8 *cmdl,
			      struct sa_cmdl_upd_info *upd_info)
{
	u8 offset = 0;
	u32 *word_ptr = (u32 *)cmdl;
	int i;
	int ret = 0;

	/* Clear the command label */
	memset(cmdl, 0, (SA_MAX_CMDL_WORDS * sizeof(u32)));

	/* Iniialize the command update structure */
	memset(upd_info, 0, sizeof(*upd_info));
	upd_info->enc_size.offset = 2;
	upd_info->enc_size.size = 2;
	upd_info->enc_offset.size = 1;
	upd_info->enc_size2.size = 4;
	upd_info->auth_size.offset = 2;
	upd_info->auth_size.size = 2;
	upd_info->auth_offset.size = 1;

	if (cfg->aalg == SA_AALG_ID_AES_XCBC) {
		/* Derive K2/K3 subkeys */
		ret = sa_aes_xcbc_subkey(NULL, (u8 *)&upd_info->aux_key[0],
					 (u8 *)&upd_info->aux_key[AES_BLOCK_SIZE
					 / sizeof(u32)],
					 cfg->akey, cfg->akey_len);
		if (ret)
			return ret;

		/*
		 * Format the key into 32bit CPU words
		 * from a big-endian stream
		 */
		for (i = 0; i < SA_MAX_AUX_DATA_WORDS; i++)
			upd_info->aux_key[i] =
				be32_to_cpu(upd_info->aux_key[i]);
	}

	if (cfg->enc1st) {
		if (cfg->enc_eng_id != SA_ENG_ID_NONE) {
			upd_info->flags |= SA_CMDL_UPD_ENC;
			upd_info->enc_size.index = 0;
			upd_info->enc_offset.index = 1;

			if ((cfg->enc_eng_id == SA_ENG_ID_EM1) &&
			    (cfg->auth_eng_id == SA_ENG_ID_EM1))
				cfg->auth_eng_id = SA_ENG_ID_EM2;

			/* Encryption command label */
			if (cfg->auth_eng_id != SA_ENG_ID_NONE)
				cmdl[SA_CMDL_OFFSET_NESC] = cfg->auth_eng_id;
			else
				cmdl[SA_CMDL_OFFSET_NESC] = SA_ENG_ID_OUTPORT2;

			/* Encryption modes requiring IV */
			if (cfg->iv_size) {
				upd_info->flags |= SA_CMDL_UPD_ENC_IV;
				upd_info->enc_iv.index =
					SA_CMDL_HEADER_SIZE_BYTES >> 2;
				upd_info->enc_iv.size = cfg->iv_size;

				cmdl[SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES +
					cfg->iv_size;

				cmdl[SA_CMDL_OFFSET_OPTION_CTRL1] =
					(SA_CTX_ENC_AUX2_OFFSET |
					 (cfg->iv_size >> 3));

				offset = SA_CMDL_HEADER_SIZE_BYTES +
						cfg->iv_size;
			} else {
				cmdl[SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES;
				offset = SA_CMDL_HEADER_SIZE_BYTES;
			}
		}

		if (cfg->auth_eng_id != SA_ENG_ID_NONE) {
			upd_info->flags |= SA_CMDL_UPD_AUTH;
			upd_info->auth_size.index = offset >> 2;
			upd_info->auth_offset.index =
				upd_info->auth_size.index + 1;

			cmdl[offset + SA_CMDL_OFFSET_NESC] = SA_ENG_ID_OUTPORT2;

			/* Algorithm with subkeys */
			if ((cfg->aalg == SA_AALG_ID_AES_XCBC) ||
			    (cfg->aalg == SA_AALG_ID_CMAC)) {
				upd_info->flags |= SA_CMDL_UPD_AUX_KEY;
				upd_info->aux_key_info.index =
				(offset + SA_CMDL_HEADER_SIZE_BYTES) >> 2;

				cmdl[offset + SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES + 16;
				cmdl[offset + SA_CMDL_OFFSET_OPTION_CTRL1] =
					(SA_CTX_ENC_AUX1_OFFSET | (16 >> 3));

				offset += SA_CMDL_HEADER_SIZE_BYTES + 16;
			} else {
				cmdl[offset + SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES;
				offset += SA_CMDL_HEADER_SIZE_BYTES;
			}
		}
	} else {
		/* Auth first */
		if (cfg->auth_eng_id != SA_ENG_ID_NONE) {
			upd_info->flags |= SA_CMDL_UPD_AUTH;
			upd_info->auth_size.index = 0;
			upd_info->auth_offset.index = 1;

			if ((cfg->auth_eng_id == SA_ENG_ID_EM1) &&
			    (cfg->enc_eng_id == SA_ENG_ID_EM1))
				cfg->enc_eng_id = SA_ENG_ID_EM2;

			/* Authentication command label */
			if (cfg->enc_eng_id != SA_ENG_ID_NONE)
				cmdl[SA_CMDL_OFFSET_NESC] = cfg->enc_eng_id;
			else
				cmdl[SA_CMDL_OFFSET_NESC] = SA_ENG_ID_OUTPORT2;

			/* Algorithm with subkeys */
			if ((cfg->aalg == SA_AALG_ID_AES_XCBC) ||
			    (cfg->aalg == SA_AALG_ID_CMAC)) {
				upd_info->flags |= SA_CMDL_UPD_AUX_KEY;
				upd_info->aux_key_info.index =
					(SA_CMDL_HEADER_SIZE_BYTES) >> 2;

				cmdl[SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES + 16;
				cmdl[offset + SA_CMDL_OFFSET_OPTION_CTRL1] =
					(SA_CTX_ENC_AUX1_OFFSET | (16 >> 3));

				offset = SA_CMDL_HEADER_SIZE_BYTES + 16;
			} else {
				cmdl[SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES;
				offset = SA_CMDL_HEADER_SIZE_BYTES;
			}
		}

		if (cfg->enc_eng_id != SA_ENG_ID_NONE) {
			upd_info->flags |= SA_CMDL_UPD_ENC;
			upd_info->enc_size.index = offset >> 2;
			upd_info->enc_offset.index =
				upd_info->enc_size.index + 1;

			cmdl[offset + SA_CMDL_OFFSET_NESC] = SA_ENG_ID_OUTPORT2;

			/* Encryption modes requiring IV */
			if (cfg->iv_size) {
				upd_info->flags |= SA_CMDL_UPD_ENC_IV;
				upd_info->enc_iv.index =
				(offset + SA_CMDL_HEADER_SIZE_BYTES) >> 2;
				upd_info->enc_iv.size = cfg->iv_size;

				cmdl[offset + SA_CMDL_OFFSET_LABEL_LEN] =
				SA_CMDL_HEADER_SIZE_BYTES + cfg->iv_size;

				cmdl[offset + SA_CMDL_OFFSET_OPTION_CTRL1] =
				(SA_CTX_ENC_AUX2_OFFSET | (cfg->iv_size >> 3));

				offset += SA_CMDL_HEADER_SIZE_BYTES +
						cfg->iv_size;
			} else {
				cmdl[offset + SA_CMDL_OFFSET_LABEL_LEN] =
					SA_CMDL_HEADER_SIZE_BYTES;
				offset += SA_CMDL_HEADER_SIZE_BYTES;
			}
		}
	}

	offset = roundup(offset, 8);

	for (i = 0; i < offset / 4; i++)
		word_ptr[i] = be32_to_cpu(word_ptr[i]);

	return offset;
}

static inline void sa_copy_iv(u32 *out, const u8 *iv, bool size16)
{
	int j;

	for (j = 0; j < ((size16) ? 4 : 2); j++) {
		*out = cpu_to_be32(*((u32 *)iv));
		iv += 4;
		out++;
	}
}

/* Update Command label */
static inline void
sa_update_cmdl(struct device *dev, u8 enc_offset, u16 enc_size,	u8 *enc_iv,
	       u8 auth_offset, u16 auth_size, u8 *auth_iv, u8 aad_size,
	       u8 *aad,	struct sa_cmdl_upd_info	*upd_info, u32 *cmdl)
{
	switch (upd_info->submode) {
	case SA_MODE_GEN:
		if (likely(upd_info->flags & SA_CMDL_UPD_ENC)) {
			cmdl[upd_info->enc_size.index] &= 0xffff0000;
			cmdl[upd_info->enc_size.index] |= enc_size;
			cmdl[upd_info->enc_offset.index] &= 0x00ffffff;
			cmdl[upd_info->enc_offset.index] |=
						((u32)enc_offset << 24);

			if (likely(upd_info->flags & SA_CMDL_UPD_ENC_IV)) {
				sa_copy_iv(&cmdl[upd_info->enc_iv.index],
					   enc_iv,
					   (upd_info->enc_iv.size > 8));
			}
		}

		if (likely(upd_info->flags & SA_CMDL_UPD_AUTH)) {
			cmdl[upd_info->auth_size.index] &= 0xffff0000;
			cmdl[upd_info->auth_size.index] |= auth_size;
			cmdl[upd_info->auth_offset.index] &= 0x00ffffff;
			cmdl[upd_info->auth_offset.index] |=
					((u32)auth_offset << 24);

			if (upd_info->flags & SA_CMDL_UPD_AUTH_IV) {
				sa_copy_iv(&cmdl[upd_info->auth_iv.index],
					   auth_iv,
					   (upd_info->auth_iv.size > 8));
			}

			if (upd_info->flags & SA_CMDL_UPD_AUX_KEY) {
				int offset = (auth_size & 0xF) ? 4 : 0;

				memcpy(&cmdl[upd_info->aux_key_info.index],
				       &upd_info->aux_key[offset], 16);
			}
		}
		break;

	case SA_MODE_CCM:
	case SA_MODE_GCM:
	case SA_MODE_GMAC:
	default:
		dev_err(dev, "unsupported mode(%d)\n", upd_info->submode);
		break;
	}
}

