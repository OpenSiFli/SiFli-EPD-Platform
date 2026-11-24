"""Generate the complete cold budget implementation for component host ports."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]

def prepare(out):
    source = (ROOT / 'src/platform/epd_cold_budget.c').read_text(encoding='utf-8')
    source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
    source = source.replace('struct stat st;', 'budget_stat_t st;')
    (out / 'cold_budget_actual.inc').write_text(source, encoding='utf-8')
