#!/usr/bin/python
import h5py
import math
import numpy as np

f = h5py.File('zsim-ev.h5', 'r')
dset = f["stats"]["root"]
stats = dset[-1]
phases = stats['phase']
coreStats = stats['c']
totalInstrs = coreStats['instrs']
totalCycles = coreStats['cycles']
olderr = np.seterr(all='ignore')
ipc= (1. * totalInstrs)/totalCycles
print "ipc = "
print ipc

arrLen = len(ipc)
#print "arrLen = ", arrLen
sum_ipc = 0
i = 0
for every_ipc in ipc:
    if math.isnan(every_ipc):
        continue
    else:
        #print "every_ipc = ", every_ipc
        sum_ipc = sum_ipc + every_ipc
        #print "sum_ipc = ", sum_ipc
        i = i+1
        #print "i = ", i
print "Valid instances num = ", i
print "Everage ipc = ", sum_ipc/i


