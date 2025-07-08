# SPDX-License-Identifier: Apache-2.0

if get_option('openpower-dumps-extension').allowed()
    subdir('openpower-dumps')
elif get_option('system-dumps-extension').allowed()
    subdir('system-dumps')
else
    subdir('default')
endif
