"""Build-only namespace for AIC's list helpers when linked with SiFli Bluetooth."""
import subprocess


LIST_SYMBOLS = tuple("co_list_" + suffix for suffix in (
    "cnt", "concat", "extract", "extract_ret", "find", "init", "insert",
    "insert_after", "insert_before", "pool_init", "pop_front", "push_back",
    "push_front", "remove", "size",
))


def renamed(symbol):
    return "epd_aic_" + symbol


def namespace_archive(objcopy, source, target):
    subprocess.run([str(objcopy), *[
        "--redefine-sym=" + symbol + "=" + renamed(symbol)
        for symbol in LIST_SYMBOLS
    ], str(source), str(target)], check=True)


def scons_action(target, source, env):
    namespace_archive(env["AIC_OBJCOPY"], source[0].abspath, target[0].abspath)
    return 0
