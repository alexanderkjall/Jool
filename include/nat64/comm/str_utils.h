#ifndef _JOOL_COMM_STR_UTILS_H
#define _JOOL_COMM_STR_UTILS_H

/**
 * @file
 * Two-liners (since you need to check the return value) for string-to-something else conversions.
 * This is only used by the parser of the user's arguments, so it's very noisy on the console on
 * purpose.
 *
 * @author Alberto Leiva
 */

#include "nat64/comm/types.h"


#define MAX_U8 0xFF
#define MAX_U16 0xFFFF
#define MAX_U32 0xFFFFFFFF
#define MAX_U64 0xFFFFFFFFFFFFFFFF

int str_to_bool(const char *str, __u8 *bool_out);
int str_to_u8(const char *str, __u8 *u8_out, __u8 min, __u8 max);
int str_to_u16(const char *str, __u16 *u16_out, __u16 min, __u16 max);
int str_to_u64(const char *str, __u64 *u64_out, __u64 min, __u64 max);
int str_to_u16_array(const char *str, __u16 **array_out, size_t *array_len_out);
/**
 * Converts "str" to a IPv4 address. Stores the result in "result".
 *
 * Useful mainly in code common to kernelspace and userspace, since their conversion functions
 * differ, but meant to be used everywhere to strip the parameters from in4_pton() we don't want.
 */
int str_to_addr4(const char *str, struct in_addr *result);
/**
 * Converts "str" to a IPv6 address. Stores the result in "result".
 *
 * Useful mainly in code common to kernelspace and userspace, since their conversion functions
 * differ, but meant to be used everywhere to strip the parameters from in6_pton() we don't want.
 */
int str_to_addr6(const char *str, struct in6_addr *result);
int str_to_addr4_port(const char *str, struct ipv4_tuple_address *addr_out);
int str_to_addr6_port(const char *str, struct ipv6_tuple_address *addr_out);
int str_to_prefix(const char *str, struct ipv6_prefix *prefix_out);

void print_time(__u64 millis);


#endif /* _JOOL_COMM_STR_UTILS_H */
