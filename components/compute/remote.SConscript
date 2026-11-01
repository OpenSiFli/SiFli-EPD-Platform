from building import *
import os

cwd = GetCurrentDir()
sources = (Glob('epd_compute_protocol.c') + Glob('epd_compute_cache.c') +
           Glob('epd_compute_remote.c') + Glob('epd_compute_memory.c') +
           Glob('epd_compute_fiber.c') + Glob('epd_compute_pipe_remote.c'))
objects = DefineGroup('compute_remote_common', sources, depend=[], CPPPATH=[os.path.join(cwd, 'include')])
Return('objects')
