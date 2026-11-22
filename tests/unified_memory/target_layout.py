# Copyright (c) 2019-2026 SiFli Technologies(Nanjing) Co., Ltd
# SPDX-License-Identifier: Apache-2.0
"""Compile one core object with the production ARM ABI and inspect its sections."""
import argparse
import json
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", required=True, type=Path)
    parser.add_argument("--project-build", type=Path,
                        default=ROOT / "project/build_sf32lb57-epd-gyd7n6_hcpu")
    args = parser.parse_args()
    build = HERE / ".build"
    build.mkdir(exist_ok=True)
    obj = build / "core-arm.o"
    sdk = ROOT / "SiFli-SDK"
    includes = [args.project_build, sdk / "rtos/rtthread/include",
                sdk / "rtos/rtthread/components/finsh", sdk / "middleware/include",
                sdk / "drivers/cmsis/sf32lb57x",
                sdk / "external/CMSIS/Include", sdk / "drivers/Include", sdk / "drivers/cmsis/Include",
                sdk / "middleware/sifli_matrix",
                ROOT / "src/platform"]
    flags = ["-std=c11", "-mcpu=cortex-m33", "-mthumb", "-mfpu=fpv5-sp-d16",
             "-mfloat-abi=hard", "-fshort-enums", "-ffunction-sections", "-fdata-sections",
             "-Os", "-fstack-usage", "-DSOC_BF0_HCPU", "-DSF32LB57X", "-DHAVE_CCONFIG_H"]
    subprocess.run([str(args.cc), "-c", *flags, *["-I" + str(p) for p in includes],
                    str(ROOT / "src/platform/epd_memory_manager.c"), "-o", str(obj)], check=True)
    objdump = args.cc.with_name(args.cc.name.replace("gcc", "objdump"))
    table = subprocess.check_output([str(objdump), "-t", str(obj)], text=True)
    symbols = {}
    for name in ("manager", "descriptors", "__rt_init_memory_descriptors_init"):
        fields = next(line.split() for line in table.splitlines() if line.endswith(" " + name))
        symbols[name] = {"section": fields[-3], "bytes": int(fields[-2], 16)}
    assert symbols["manager"]["section"] == ".bss.manager", symbols
    assert symbols["descriptors"]["section"] == ".bss.l2_ret_bss_epd_memory_manager", symbols
    assert symbols["__rt_init_memory_descriptors_init"]["section"] == ".rti_fn.25", symbols
    result = {"arm_object": symbols,
              "metadata_bytes": sum(symbols[n]["bytes"] for n in ("manager", "descriptors"))}
    bridge = build / 'sdk-arm.o'
    subprocess.run([str(args.cc), '-c', *flags, *['-I'+str(p) for p in includes],
                    str(ROOT/'src/platform/epd_memory_sdk.c'), '-o', str(bridge)], check=True)
    bridge_symbols = subprocess.check_output([str(objdump), '-t', str(bridge)], text=True)
    assert '__rtmsym_epd_memory_app_backend' in bridge_symbols, bridge_symbols
    assert '__rtmsym_epd_memory_block_get' in bridge_symbols, bridge_symbols
    assert '__rtmsym_epd_app_allocation_peak' in bridge_symbols, bridge_symbols
    for function in ('begin', 'poll', 'failed'):
        assert '__rtmsym_epd_memory_recovery_' + function in bridge_symbols, bridge_symbols
    assert '__fsym___cmd_mem_diag' in bridge_symbols, bridge_symbols
    result['arm_sdk_backend_export'] = True
    result['arm_sdk_block_query_export'] = True
    result['arm_sdk_allocation_peak_export'] = True
    result['arm_sdk_recovery_exports'] = True
    result['arm_msh_diagnostic_export'] = True
    memheap_hooks = []
    for hooks in (False, True):
        source = build / ('memheap-arm-hooks-on.c' if hooks else 'memheap-arm-hooks-off.c')
        source.write_text('#include <rtconfig.h>\n' +
                          ('#undef RT_USING_HOOK\n' if not hooks else '') +
                          '#include "' + (sdk / 'rtos/rtthread/src/memheap.c').as_posix() + '"\n')
        heap_object = source.with_suffix('.o')
        subprocess.run([str(args.cc), '-c', *flags, *['-I'+str(p) for p in includes],
                        str(source), '-o', str(heap_object)], check=True)
        heap_symbols = subprocess.check_output([str(objdump), '-t', str(heap_object)], text=True)
        assert any(line.endswith(' rt_memheap_alloc') and '*UND*' not in line
                   for line in heap_symbols.splitlines())
        memheap_hooks.append(hooks)
    result['arm_memheap_diagnostic_hook_variants'] = memheap_hooks
    stack = []
    for line in obj.with_suffix(".su").read_text().splitlines():
        function, size, kind = line.split("\t")
        stack.append({"function": function.rsplit(":",1)[-1], "bytes": int(size), "kind": kind})
    result["arm_function_stack"] = sorted(stack,key=lambda row:-row["bytes"])
    # Compare the real SDK item ABI, retaining all other production switches.
    sizes = {}
    thread_sizes = {}
    for enabled in (False, True):
        source = build / ("heap-header-on.c" if enabled else "heap-header-off.c")
        source.write_text('#include <rtconfig.h>\n' +
                          '#undef RT_USING_MEMHEAP_OWNER\n' +
                          '#undef RT_USING_THREAD_MEM_CONTEXT\n' +
                          ('#define RT_USING_MEMHEAP_OWNER 1\n' if enabled else '') +
                          ('#define RT_USING_THREAD_MEM_CONTEXT 1\n' if enabled else '') +
                          '#include <rtthread.h>\nunsigned char heap_item_size[RT_ALIGN(sizeof(struct rt_memheap_item),RT_ALIGN_SIZE)];\n'
                          'unsigned char thread_context_size[sizeof(struct rt_thread)];\n')
        item = source.with_suffix('.o')
        subprocess.run([str(args.cc), '-c', *flags, *['-I'+str(p) for p in includes],
                        str(source),'-o',str(item)],check=True)
        table = subprocess.check_output([str(objdump),'-t',str(item)],text=True)
        fields = next(line.split() for line in table.splitlines() if line.endswith(' heap_item_size'))
        sizes['owner_on' if enabled else 'owner_off'] = int(fields[-2],16)
        fields = next(line.split() for line in table.splitlines() if line.endswith(' thread_context_size'))
        thread_sizes['owner_on' if enabled else 'owner_off'] = int(fields[-2],16)
    sizes['increment_per_heap_item'] = sizes['owner_on'] - sizes['owner_off']
    result['arm_heap_header'] = sizes
    thread_sizes['increment_per_thread'] = thread_sizes['owner_on'] - thread_sizes['owner_off']
    result['arm_thread_context'] = thread_sizes
    map_path = args.project_build / "main.map"
    if map_path.exists():
        content = map_path.read_text()
        memory = {}
        for name in ("RAM", "PSRAM"):
            match = re.search(r"^" + name + r"\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)", content, re.M)
            if match:
                memory[name] = {"origin": int(match[1], 16), "length": int(match[2], 16)}
        end = re.search(r"0x([0-9a-f]+)\s+__bss_end__ =", content)
        if end and "RAM" in memory:
            memory["sram_bss_end"] = int(end[1], 16)
            memory["sram_overflow"] = max(0, int(end[1], 16) -
                                           memory["RAM"]["origin"] - memory["RAM"]["length"])
        rows = []
        for match in re.finditer(r"^ (\.bss\S*)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+) (.+)$", content, re.M):
            address, size = int(match[2], 16), int(match[3], 16)
            if size and 0x20000000 <= address < 0x20100000 and "src\\" in match[4]:
                rows.append({"section": match[1], "address": hex(address), "bytes": size,
                             "object": match[4]})
        result["existing_map"] = {"path": str(map_path), "mtime": map_path.stat().st_mtime,
                                  "memory": memory,
                                  "application_sram": sorted(rows, key=lambda row: -row["bytes"])}
    (build / "layout.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({k: v for k, v in result.items() if k not in ("existing_map","arm_function_stack")}, indent=2))
    print('Largest individual stack frames (call chains require separate accounting):', result['arm_function_stack'][:6])
    if "existing_map" in result:
        print("Existing map (may predate this object):", result["existing_map"]["memory"])


if __name__ == "__main__":
    main()
