#include <rtthread.h>
#include <rtm.h>
#include <ulog.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <dfs_file.h>
#include <ff.h>
#include <errno.h>
#include <ctype.h>
#include <cJSON.h>
#include <tls_client.h>
#include <miniz.h>
#include <lwip/api.h>
#include "storage.h"

/* Compiler double helpers use base AAPCS even in a hard-float image. */
extern int __attribute__((pcs("aapcs"))) __aeabi_d2iz(double);
extern unsigned int __attribute__((pcs("aapcs"))) __aeabi_d2uiz(double);
extern double __attribute__((pcs("aapcs"))) __aeabi_dadd(double, double);
extern double __attribute__((pcs("aapcs"))) __aeabi_dsub(double, double);
extern double __attribute__((pcs("aapcs"))) __aeabi_dmul(double, double);
extern double __attribute__((pcs("aapcs"))) __aeabi_i2d(int);
extern double __attribute__((pcs("aapcs"))) __aeabi_ui2d(unsigned int);
extern int __attribute__((pcs("aapcs"))) __aeabi_dcmpeq(double, double);
extern int __attribute__((pcs("aapcs"))) __aeabi_dcmple(double, double);
extern int __attribute__((pcs("aapcs"))) __aeabi_dcmpge(double, double);
extern int __attribute__((pcs("aapcs"))) __aeabi_dcmpgt(double, double);
extern int __attribute__((pcs("aapcs"))) __aeabi_dcmplt(double, double);
extern int __attribute__((pcs("aapcs"))) __aeabi_dcmpun(double, double);
/* Register-only libgcc div/mod ABI: export the address, never call it from C. */
extern void __aeabi_uldivmod(void);
extern void __aeabi_ldivmod(void);

RTM_EXPORT(__aeabi_d2iz);
RTM_EXPORT(__aeabi_d2uiz);
RTM_EXPORT(__aeabi_dadd);
RTM_EXPORT(__aeabi_dcmpge);
RTM_EXPORT(__aeabi_dcmpgt);
RTM_EXPORT(__aeabi_dcmplt);
RTM_EXPORT(__aeabi_dcmpun);
RTM_EXPORT(__aeabi_dmul);
RTM_EXPORT(__aeabi_dsub);
RTM_EXPORT(__aeabi_i2d);
RTM_EXPORT(__aeabi_ui2d);
RTM_EXPORT(__aeabi_dcmpeq);
RTM_EXPORT(__aeabi_dcmple);
RTM_EXPORT(__errno);
RTM_EXPORT(_ctype_);
RTM_EXPORT(atoi);
RTM_EXPORT(strtod);
RTM_EXPORT(cJSON_Delete);
RTM_EXPORT(cJSON_GetArrayItem);
RTM_EXPORT(cJSON_GetArraySize);
RTM_EXPORT(cJSON_GetObjectItemCaseSensitive);
RTM_EXPORT(cJSON_IsArray);
RTM_EXPORT(cJSON_IsNumber);
RTM_EXPORT(cJSON_IsObject);
RTM_EXPORT(cJSON_IsString);
RTM_EXPORT(cJSON_ParseWithOpts);
RTM_EXPORT(mbedtls_client_close);
RTM_EXPORT(mbedtls_client_context);
RTM_EXPORT(mbedtls_client_init);
RTM_EXPORT(mbedtls_net_recv);
RTM_EXPORT(mbedtls_net_send);
RTM_EXPORT(mbedtls_ssl_get_verify_result);
RTM_EXPORT(mbedtls_ssl_handshake);
RTM_EXPORT(mbedtls_ssl_read);
RTM_EXPORT(mbedtls_ssl_set_bio);
RTM_EXPORT(mbedtls_ssl_write);
RTM_EXPORT(mz_crc32);
RTM_EXPORT(netconn_gethostbyname);
RTM_EXPORT(rt_memory_info);
RTM_EXPORT(storage_available);
RTM_EXPORT(storage_changing);
RTM_EXPORT(tinfl_decompress);
RTM_EXPORT(tinfl_decompressor_alloc);
RTM_EXPORT(tinfl_decompressor_free);
RTM_EXPORT(ulog_output);
RTM_EXPORT(__aeabi_uldivmod);
RTM_EXPORT(__aeabi_ldivmod);
RTM_EXPORT(ff_oem2uni);
RTM_EXPORT(localtime_r);
RTM_EXPORT(strftime);
RTM_EXPORT(strrchr);
RTM_EXPORT(storage_path_root);
RTM_EXPORT(storage_relative_path);
RTM_EXPORT(storage_revision);
RTM_EXPORT(storage_root);
RTM_EXPORT(storage_info);
RTM_EXPORT(dfs_file_open);
RTM_EXPORT(dfs_file_close);
RTM_EXPORT(dfs_file_read);
RTM_EXPORT(dfs_file_lseek);
RTM_EXPORT(vsnprintf);
RTM_EXPORT(sscanf);
RTM_EXPORT(qsort);
RTM_EXPORT(bsearch);
RTM_EXPORT(mktime);
RTM_EXPORT(strpbrk);
