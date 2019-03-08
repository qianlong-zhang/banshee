#!/usr/bin/python
import h5py
import numpy as np
f = h5py.File('zsim-ev.h5', 'r')
dset = f["stats"]["root"]
stats = dset[-1]
phases = stats['phase']
coreStats = stats['c']
totalInstrs = coreStats['instrs']
totalCycles = coreStats['cycles']
ipc= (1. * totalInstrs)/totalCycles
print "ipc = "
print ipc
