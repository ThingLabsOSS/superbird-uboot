// SPDX-License-Identifier: GPL-2.0+
#ifndef __CARTHING_CERT_PARSE_H
#define __CARTHING_CERT_PARSE_H

#include <linux/types.h>

struct carthing_cert_info {
	char issuer_cn[64];	/* DN's last CN — usually the Apple CA name */
	char subject_cn[64];	/* DN's last CN — "IPA_<uid>" for accessory */
	char not_before[20];	/* raw ASN.1 time string (YYMMDDHHMMSSZ or longer) */
	char not_after[20];
};

/* Parse the carthing's PKCS#7-wrapped MFi X.509 cert.
 * Returns 0 on success, negative on parse failure. info is always
 * zeroed at entry. */
int carthing_cert_parse(const uint8_t *cert, size_t cert_len,
			struct carthing_cert_info *info);

#endif /* __CARTHING_CERT_PARSE_H */
