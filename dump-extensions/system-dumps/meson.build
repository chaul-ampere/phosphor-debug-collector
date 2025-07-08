# SPDX-License-Identifier: Apache-2.0

unit_files += {'input': 'dump-extensions/system-dumps/system-dump-handling@.service',
         'output': 'system-dump-handling@.service'}

# Configuration header file(system_dumps_config.h) generation
opconf_data = configuration_data()

opconf_data.set_quoted('SYSTEM_DUMP_OBJPATH', get_option('SYSTEM_DUMP_OBJPATH'),
                      description : 'The system dump manager D-Bus path'
                    )
opconf_data.set_quoted('SYSTEM_DUMP_OBJ_ENTRY', get_option('SYSTEM_DUMP_OBJ_ENTRY'),
                      description : 'The system dump entry D-Bus object path'
                    )

opconf_data.set_quoted('SYSTEM_DUMP_PATH', get_option('SYSTEM_DUMP_PATH'),
                      description : 'The system dump entry D-Bus  path'
                    )

configure_file(configuration : opconf_data,
               output : 'system_dumps_config.h'
              )

phosphor_dump_manager_sources += [
        'dump-extensions/system-dumps/dump-extensions.cpp',
        'dump-extensions/system-dumps/dump_manager_system.cpp',
        'dump-extensions/system-dumps/system_dump_entry.cpp',
    ]