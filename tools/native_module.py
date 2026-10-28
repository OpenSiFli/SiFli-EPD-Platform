"""Load the selected firmware configuration before SDK BuildLibrary runs."""
import importlib.util
from pathlib import Path
import sys

from i18n import configure_module, without_platform_tables


def configure(repo, firmware, module_dir=None):
    repo, firmware = Path(repo).resolve(), Path(firmware).resolve()
    for name in ('rtconfig.h', 'rtua.py'):
        if not (firmware / name).is_file():
            raise ValueError('Build the platform first: missing ' + str(firmware / name))
    spec = importlib.util.spec_from_file_location('rtua', firmware / 'rtua.py')
    configuration = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(configuration)
    if module_dir is not None:
        configure_module(configuration, repo, firmware, module_dir)
    else:
        original_paths = configuration.GetCPPPATH

        def paths(bsp_root, rtt_root):
            result = [str(firmware)] + original_paths(str(repo / 'project'), rtt_root)
            result += [str(repo / 'SiFli-SDK' / path.replace('\\', '/').split('/sifli_sdk/', 1)[1])
                       for path in result if '/sifli_sdk/' in path.replace('\\', '/')]
            return without_platform_tables(result)

        configuration.GetCPPPATH = paths
    sys.modules['rtua'] = configuration
