/* SPDX-License-Identifier: Apache-2.0 */
#ifndef BOOK_COMPUTE_XML_PORT_H
#define BOOK_COMPUTE_XML_PORT_H
/* Force-include for libxml2 translation units only. book_xml_port.h already
 * disables file reads; also remove the residual native filesystem probe. */
#include <sys/stat.h>
static inline int book_compute_xml_stat(const char *path, struct stat *result)
{
    (void)path;
    (void)result;
    return -1;
}
#define stat(path, result) book_compute_xml_stat(path, result)
#endif
