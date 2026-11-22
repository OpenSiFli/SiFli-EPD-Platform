"""Exercise persistent cover I/O and the production bookshelf publication path."""
from pathlib import Path
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "books_scan_perf"))
from run import run
sys.path.insert(0, str(HERE.parent / "storage_perf"))
from owned_fixture import write_owned_fixture
sys.path.insert(0, str(HERE.parent / "cache_budget"))
from fixture import prepare as prepare_budget
def prepare(out):
    write_owned_fixture(out)
    prepare_budget(out)

run(HERE, {"bookshelf.inc": "modules/books/bookshelf.c",
           "shelf_data.inc": "modules/books/ui_bookshelf_data.c",
           "cover_cache.inc": "modules/books/cover_cache.c"}, prepare=prepare)
