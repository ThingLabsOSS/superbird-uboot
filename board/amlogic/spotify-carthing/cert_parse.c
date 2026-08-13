// SPDX-License-Identifier: GPL-2.0+
/*
 * Minimal X.509 ASN.1 parser tailored to the Apple MFi accessory cert.
 *
 * Not a general-purpose ASN.1 library — it just walks the carthing's
 * MFi cert (PKCS#7 SignedData envelope wrapping a single X.509) and
 * pulls out:
 *   - issuer Common Name (DN CN attribute)
 *   - subject Common Name
 *   - validity period (notBefore, notAfter as UTCTime / GeneralizedTime)
 */
#include <linux/string.h>
#include <linux/types.h>

#include "cert_parse.h"

#define TAG_INTEGER	0x02
#define TAG_BITSTRING	0x03
#define TAG_OCTETSTRING	0x04
#define TAG_OID		0x06
#define TAG_UTF8STRING	0x0C
#define TAG_PRINTABLE	0x13
#define TAG_TELETEX	0x14
#define TAG_IA5STRING	0x16
#define TAG_UTCTIME	0x17
#define TAG_GENERALTIME	0x18
#define TAG_SEQUENCE	0x30
#define TAG_SET		0x31

/* OID 2.5.4.3 = commonName, DER-encoded as bytes after the tag/len. */
static const uint8_t OID_CN[] = { 0x55, 0x04, 0x03 };

/* Decode an ASN.1 length. Returns number of bytes consumed (1..3) or
 * -1 on overrun. *len_out receives the parsed length. */
static int asn1_len(const uint8_t *p, size_t avail, size_t *len_out)
{
	uint8_t b;

	if (avail < 1)
		return -1;
	b = p[0];
	if ((b & 0x80) == 0) {
		*len_out = b;
		return 1;
	}
	if (b == 0x81) {
		if (avail < 2)
			return -1;
		*len_out = p[1];
		return 2;
	}
	if (b == 0x82) {
		if (avail < 3)
			return -1;
		*len_out = ((size_t)p[1] << 8) | p[2];
		return 3;
	}
	return -1;	/* longer lengths not used in this cert */
}

/* Scan `buf` for the byte sequence `pat` (length plen). Returns offset
 * of first match, or -1 if not found. */
static int find_bytes(const uint8_t *buf, size_t buf_len,
		      const uint8_t *pat, size_t plen)
{
	size_t i;

	if (plen == 0 || plen > buf_len)
		return -1;
	for (i = 0; i + plen <= buf_len; i++) {
		if (!memcmp(buf + i, pat, plen))
			return (int)i;
	}
	return -1;
}

/*
 * Inside a DN (Name), the LAST commonName attribute is conventionally
 * the "most specific" one (e.g. "Apple Accessories Cert. Auth. - ...").
 * Scan for all `06 03 55 04 03` (CN OID) occurrences within `region`
 * and take the value after the last one.
 *
 * Each DN entry is:
 *   SET { SEQUENCE { OID(CN), <stringtag len value> } }
 *
 * So after the CN OID's value, the next TLV is the CN string.
 */
static int extract_last_cn(const uint8_t *region, size_t region_len,
			   char *out, size_t out_sz)
{
	int last_off = -1;
	size_t i;

	for (i = 0; i + 5 <= region_len; i++) {
		if (region[i] == TAG_OID && region[i + 1] == 3 &&
		    !memcmp(region + i + 2, OID_CN, 3))
			last_off = (int)i;
	}
	if (last_off < 0)
		return -1;

	/* String TLV follows immediately after the OID (5 bytes). */
	{
		size_t pos = last_off + 5;
		size_t slen;
		int hdr_len;

		if (pos + 2 > region_len)
			return -1;
		/* Accept PrintableString / UTF8String / Teletex / IA5. */
		uint8_t tag = region[pos++];
		if (tag != TAG_PRINTABLE && tag != TAG_UTF8STRING &&
		    tag != TAG_TELETEX && tag != TAG_IA5STRING)
			return -1;

		hdr_len = asn1_len(region + pos, region_len - pos, &slen);
		if (hdr_len < 0)
			return -1;
		pos += hdr_len;
		if (pos + slen > region_len)
			return -1;

		if (slen >= out_sz)
			slen = out_sz - 1;
		memcpy(out, region + pos, slen);
		out[slen] = '\0';
		return (int)slen;
	}
}

/*
 * The cert is PKCS#7-wrapped, so dig out the inner X.509 before scanning for
 * issuer / validity / subject. Rather than parse the envelope, find the v3
 * version marker `a0 03 02 01 02` and walk BACKWARD to the SEQUENCE header
 * that wraps it — the header sits 3 or 4 bytes back depending on whether the
 * length is 0x81- or 0x82-encoded. Robust for this specific issuer-signed
 * format, and nothing more.
 * Envelope structure + the alternatives: superbird-docs/hardware/i2c-devices.md.
 */
static int find_x509(const uint8_t *cert, size_t cert_len,
		     const uint8_t **x509_out, size_t *x509_len_out)
{
	/* The inner X.509 TBSCert starts with `a0 03 02 01 02` (version
	 * tag + v3 marker) at the start of its TBS. We scan for the
	 * PRECEDING SEQUENCE that wraps the whole Certificate (cert =
	 * SEQ { TBS, sigAlg, sigValue }). */
	const uint8_t version_marker[] = { 0xa0, 0x03, 0x02, 0x01, 0x02 };
	int off = find_bytes(cert, cert_len, version_marker, sizeof(version_marker));
	int cert_start, tbs_len_hdr;
	size_t tbs_len;

	if (off < 4)
		return -1;
	/* The TBS SEQUENCE header precedes the version marker. With a
	 * 3-byte length encoding it's at offset off-4 (tag) ... off-1
	 * (length bytes). With 2-byte length it's at off-3 ... off-1. */
	if (off >= 4 && cert[off - 4] == TAG_SEQUENCE && cert[off - 3] == 0x82)
		tbs_len_hdr = 4;
	else if (off >= 3 && cert[off - 3] == TAG_SEQUENCE && cert[off - 2] == 0x81)
		tbs_len_hdr = 3;
	else
		return -1;

	cert_start = off - tbs_len_hdr;
	if (asn1_len(cert + cert_start + 1, cert_len - cert_start - 1,
		     &tbs_len) < 0)
		return -1;
	*x509_out = cert + cert_start;
	*x509_len_out = (size_t)tbs_len_hdr + tbs_len;	/* includes header */
	return 0;
}

/*
 * Inside the TBSCertificate find: issuer (Name), validity (SEQ),
 * subject (Name). The TBS layout per RFC 5280:
 *
 *   version [0] EXPL INT
 *   serialNumber INT
 *   signature AlgorithmIdentifier SEQ
 *   issuer Name SEQ        <-- first SEQ after sig
 *   validity SEQ           <-- second SEQ (two times)
 *   subject Name SEQ       <-- third SEQ
 *   subjectPublicKeyInfo
 *
 * We walk by SEQ tags, skipping the algorithm SEQ first.
 */
int carthing_cert_parse(const uint8_t *cert, size_t cert_len,
			struct carthing_cert_info *info)
{
	const uint8_t *x509;
	size_t x509_len;
	size_t pos = 0;
	size_t inner;
	int hdr;
	const uint8_t *tbs;
	size_t tbs_len;
	int seq_seen = 0;
	int found_validity_off = -1;

	memset(info, 0, sizeof(*info));
	if (find_x509(cert, cert_len, &x509, &x509_len) < 0)
		return -1;

	/* x509 = SEQUENCE { TBS, sigAlg, sigValue }. Step into TBS. */
	if (x509_len < 4 || x509[0] != TAG_SEQUENCE)
		return -1;
	hdr = asn1_len(x509 + 1, x509_len - 1, &inner);
	if (hdr < 0 || 1 + (size_t)hdr + inner > x509_len)
		return -1;
	tbs = x509 + 1 + hdr;
	tbs_len = inner;
	pos = 0;

	/* Skip version [0] EXPL and serialNumber and signature alg. */
	/* version: a0 LL ... — skip 1 + lenbytes + len */
	if (pos + 2 <= tbs_len && tbs[pos] == 0xa0) {
		size_t l;
		int lh = asn1_len(tbs + pos + 1, tbs_len - pos - 1, &l);
		if (lh < 0)
			return -1;
		pos += 1 + lh + l;
	}
	/* serialNumber INT */
	if (pos + 2 <= tbs_len && tbs[pos] == TAG_INTEGER) {
		size_t l;
		int lh = asn1_len(tbs + pos + 1, tbs_len - pos - 1, &l);
		if (lh < 0)
			return -1;
		pos += 1 + lh + l;
	}
	/* signature AlgorithmIdentifier SEQ */
	if (pos + 2 <= tbs_len && tbs[pos] == TAG_SEQUENCE) {
		size_t l;
		int lh = asn1_len(tbs + pos + 1, tbs_len - pos - 1, &l);
		if (lh < 0)
			return -1;
		pos += 1 + lh + l;
	}

	/* Now: issuer SEQ, validity SEQ, subject SEQ. */
	while (pos + 2 <= tbs_len && seq_seen < 3) {
		size_t l;
		int lh;

		if (tbs[pos] != TAG_SEQUENCE)
			break;
		lh = asn1_len(tbs + pos + 1, tbs_len - pos - 1, &l);
		if (lh < 0)
			break;
		if (seq_seen == 0)
			extract_last_cn(tbs + pos + 1 + lh, l,
					info->issuer_cn, sizeof(info->issuer_cn));
		else if (seq_seen == 1)
			found_validity_off = (int)(pos + 1 + lh);
		else if (seq_seen == 2)
			extract_last_cn(tbs + pos + 1 + lh, l,
					info->subject_cn, sizeof(info->subject_cn));
		pos += 1 + lh + l;
		seq_seen++;
	}

	/* Validity contains two times in sequence. Look for UTCTime
	 * (0x17 len=13) or GeneralizedTime (0x18 len=15). */
	if (found_validity_off >= 0) {
		size_t vp = (size_t)found_validity_off;
		int times = 0;
		char *dst;

		while (vp + 2 <= tbs_len && times < 2) {
			uint8_t t = tbs[vp];
			size_t l;
			int lh;

			if (t != TAG_UTCTIME && t != TAG_GENERALTIME) {
				vp++;
				continue;
			}
			lh = asn1_len(tbs + vp + 1, tbs_len - vp - 1, &l);
			if (lh < 0)
				break;
			dst = times == 0 ? info->not_before : info->not_after;
			if (l < sizeof(info->not_before)) {
				memcpy(dst, tbs + vp + 1 + lh, l);
				dst[l] = '\0';
			}
			vp += 1 + lh + l;
			times++;
		}
	}

	return 0;
}
