macio_ss = ss.source_set()
macio_ss.add(files('macio.c'))
macio_ss.add(when: 'CONFIG_CUDA', if_true: files('cuda.c'))
macio_ss.add(when: 'CONFIG_MACIO_GPIO', if_true: files('gpio.c'))
macio_ss.add(when: 'CONFIG_MAC_DBDMA', if_true: files('mac_dbdma.c'))
macio_ss.add(when: 'CONFIG_MAC_PMU', if_true: files('pmu.c'))
macio_ss.add(when: 'CONFIG_MAC_K2_SOUND',
             if_true: files('k2_sound.c', 'tas3004.c', 'cs8420.c'))

system_ss.add_all(when: 'CONFIG_MACIO', if_true: macio_ss)
