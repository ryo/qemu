nvmm_ss = ss.source_set()
nvmm_ss.add(files(
  'nvmm-accel-ops.c',
  'nvmm-all.c',
))

specific_ss.add_all(when: 'CONFIG_NVMM', if_true: nvmm_ss)
