# SPDX-License-Identifier: Apache-2.0

phosphor_dump_manager_incdir += ['dump-collectors']
if get_option('pldm-file-transfer').allowed()
    subdir('pldm')
else
    phosphor_dump_manager_sources += ['dump-collectors/system_dump_collector.cpp']
endif
