# SPDX-License-Identifier: Apache-2.0

phosphor_dump_manager_sources += ['dump-collectors/pldm/common.cpp']
phosphor_dump_manager_incdir += ['dump-collectors/pldm']
if get_option('ampere-dump-collectors').allowed()
    phosphor_dump_manager_sources += ['dump-collectors/pldm/oem/ampere/pldm_system_dump_collector.cpp']
    phosphor_dump_manager_incdir += ['dump-collectors/pldm/oem/ampere']
else
    phosphor_dump_manager_sources += ['dump-collectors/pldm/default/pldm_system_dump_collector.cpp']
    phosphor_dump_manager_incdir += ['dump-collectors/pldm/default']
endif
