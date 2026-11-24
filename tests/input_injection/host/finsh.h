/* SPDX-FileCopyrightText: 2026 SiFli Technologies(Nanjing) Co., Ltd
 * SPDX-License-Identifier: Apache-2.0 */
#define MSH_CMD_EXPORT(name, description) int host_shell(int argc, char **argv) { return name(argc, argv); }
