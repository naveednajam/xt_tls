#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/skbuff.h>
#include <linux/netfilter/x_tables.h>
#include <linux/netfilter_ipv4/ip_tables.h>
#include <linux/netfilter_ipv6/ip6_tables.h>
#include <linux/string.h>
#include <linux/ip.h>
#include <linux/tcp.h>
#include <linux/inet.h>
#include <asm/errno.h>
#include <linux/glob.h>

#include "xt_ssl.h"

/*
 * Searches through skb->data and looks for a
 * client or server handshake. A client
 * handshake is preferred as the SNI
 * field tells us what domain the client
 * wants to connect to.
 */
static int get_ssl_hostname(const struct sk_buff *skb, char **dest)
{
	struct tcphdr *tcp_header;
	char *data, *tail;
	size_t data_len;
	u_int16_t ssl_header_len;
	u_int8_t handshake_protocol;

	tcp_header = (struct tcphdr *)skb_transport_header(skb);
	// I'm not completely sure how this works (courtesy of StackOverflow), but it works
	data = (char *)((unsigned char *)tcp_header + (tcp_header->doff * 4));
	tail = skb_tail_pointer(skb);
	// Calculate packet data length
	data_len = (uintptr_t)tail - (uintptr_t)data;

	// If this isn't an SSL handshake, abort
	if (data[0] != 0x16) {
		return EPROTO;
	}

	ssl_header_len = (data[3] << 8) + data[4] + 5;
	handshake_protocol = data[5];

	// Even if we don't have all the data, try matching anyway
	if (ssl_header_len > data_len)
		ssl_header_len = data_len;

	if (ssl_header_len > 4) {
		// Check only client hellos for now
		if (handshake_protocol == 0x01) {
			u_int offset, base_offset = 43, extension_offset = 2;
			u_int16_t session_id_len, cipher_len, compression_len, extensions_len;

			if (base_offset + 2 > data_len) {
#ifdef XT_SSL_DEBUG
				printk("[xt_ssl] Data length is to small (%d)\n", (int)data_len);
#endif
				return EPROTO;
			}

			// Get the length of the session ID
			session_id_len = data[base_offset];

#ifdef XT_SSL_DEBUG
			printk("[xt_ssl] Session ID length: %d\n", session_id_len);
#endif
			if ((session_id_len + base_offset + 2) > ssl_header_len) {
#ifdef XT_SSL_DEBUG
				printk("[xt_ssl] SSL header length is smaller than session_id_len + base_offset +2 (%d > %d)\n", (session_id_len + base_offset + 2), ssl_header_len);
#endif
				return EPROTO;
			}

			// Get the length of the ciphers
			memcpy(&cipher_len, &data[base_offset + session_id_len + 1], 2);
			cipher_len = ntohs(cipher_len);
			offset = base_offset + session_id_len + cipher_len + 2;
#ifdef XT_SSL_DEBUG
			printk("[xt_ssl] Cipher len: %d\n", cipher_len);
			printk("[xt_ssl] Offset (1): %d\n", offset);
#endif
			if (offset > ssl_header_len) {
#ifdef XT_SSL_DEBUG
				printk("[xt_ssl] SSL header length is smaller than offset (%d > %d)\n", offset, ssl_header_len);
#endif
				return EPROTO;
			}

			// Get the length of the compression types
			compression_len = data[offset + 1];
			offset += compression_len + 2;
#ifdef XT_SSL_DEBUG
			printk("[xt_ssl] Compression length: %d\n", compression_len);
			printk("[xt_ssl] Offset (2): %d\n", offset);
#endif
			if (offset > ssl_header_len) {
#ifdef XT_SSL_DEBUG
				printk("[xt_ssl] SSL header length is smaller than offset w/compression (%d > %d)\n", offset, ssl_header_len);
#endif
				return EPROTO;
			}

			// Get the length of all the extensions
			memcpy(&extensions_len, &data[offset], 2);
			extensions_len = ntohs(extensions_len);
#ifdef XT_SSL_DEBUG
			printk("[xt_ssl] Extensions length: %d\n", extensions_len);
#endif

			if ((extensions_len + offset) > ssl_header_len) {
#ifdef XT_SSL_DEBUG
				printk("[xt_ssl] SSL header length is smaller than offset w/extensions (%d > %d)\n", (extensions_len + offset), ssl_header_len);
#endif
				return EPROTO;
			}

			// Loop through all the extensions to find the SNI extension
			while (extension_offset < extensions_len)
			{
				u_int16_t extension_id, extension_len;

				memcpy(&extension_id, &data[offset + extension_offset], 2);
				extension_offset += 2;

				memcpy(&extension_len, &data[offset + extension_offset], 2);
				extension_offset += 2;

				extension_id = ntohs(extension_id), extension_len = ntohs(extension_len);

#ifdef XT_SSL_DEBUG
				printk("[xt_ssl] Extension ID: %d\n", extension_id);
				printk("[xt_ssl] Extension length: %d\n", extension_len);
#endif

				if (extension_id == 0) {
					u_int16_t name_length, name_type;

					// We don't need the server name list length, so skip that
					extension_offset += 2;
					// We don't really need name_type at the moment
					// as there's only one type in the RFC-spec.
					// However I'm leaving it in here for
					// debugging purposes.
					name_type = data[offset + extension_offset];
					extension_offset += 1;

					memcpy(&name_length, &data[offset + extension_offset], 2);
					name_length = ntohs(name_length);
					extension_offset += 2;

#ifdef XT_SSL_DEBUG
					printk("[xt_ssl] Name type: %d\n", name_type);
					printk("[xt_ssl] Name length: %d\n", name_length);
#endif
					// Allocate an extra byte for the null-terminator
					*dest = kmalloc(name_length + 1, GFP_KERNEL);
					strncpy(*dest, &data[offset + extension_offset], name_length);
					// Make sure the string is always null-terminated.
					(*dest)[name_length] = 0;

					return 0;
				}

				extension_offset += extension_len;
			}
		}
	}

	return EPROTO;
}

static bool ssl_mt(const struct sk_buff *skb, struct xt_action_param *par)
{
	char *parsed_host;
	const struct xt_ssl_info *info = par->matchinfo;
	int result;
	bool invert = (info->invert & XT_SSL_OP_HOST);
	bool match;

	if ((result = get_ssl_hostname(skb, &parsed_host)) != 0)
		return false;

	match = glob_match(info->ssl_host, parsed_host);

#ifdef XT_SSL_DEBUG
	printk("[xt_ssl] Parsed domain: %s\n", parsed_host);
	printk("[xt_ssl] Domain matches: %s, invert: %s\n", match ? "true" : "false", invert ? "true" : "false");
#endif
	if (invert)
		match = !match;

	kfree(parsed_host);

	return match;
}

static int ssl_mt_check (const struct xt_mtchk_param *par)
{
	__u16 proto;

	if (par->family == NFPROTO_IPV4) {
		proto = ((const struct ipt_ip *) par->entryinfo)->proto;
	} else if (par->family == NFPROTO_IPV6) {
		proto = ((const struct ip6t_ip6 *) par->entryinfo)->proto;
	} else {
		return -EINVAL;
	}

	if (proto != IPPROTO_TCP) {
		pr_info("Can be used only in combination with "
			"-p tcp\n");
		return -EINVAL;
	}

	return 0;
}

static struct xt_match ssl_mt_regs[] __read_mostly = {
	{
		.name       = "ssl",
		.revision   = 0,
		.family     = NFPROTO_IPV4,
		.checkentry = ssl_mt_check,
		.match      = ssl_mt,
		.matchsize  = sizeof(struct xt_ssl_info),
		.me         = THIS_MODULE,
	},
#if IS_ENABLED(CONFIG_IP6_NF_IPTABLES)
	{
		.name       = "ssl",
		.revision   = 0,
		.family     = NFPROTO_IPV6,
		.checkentry = ssl_mt_check,
		.match      = ssl_mt,
		.matchsize  = sizeof(struct xt_ssl_info),
		.me         = THIS_MODULE,
	},
#endif
};

static int __init ssl_mt_init (void)
{
	return xt_register_matches(ssl_mt_regs, ARRAY_SIZE(ssl_mt_regs));
}

static void __exit ssl_mt_exit (void)
{
	xt_unregister_matches(ssl_mt_regs, ARRAY_SIZE(ssl_mt_regs));
}

module_init(ssl_mt_init);
module_exit(ssl_mt_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nils Andreas Svee <nils@stokkdalen.no>");
MODULE_DESCRIPTION("Xtables: SSL (SNI) matching");
MODULE_ALIAS("ipt_ssl");
