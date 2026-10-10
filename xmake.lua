set_xmakever('3.0.1')
includes('lib/commonlibsse-ng')

set_project('CombatGroupDeadlockFix')
set_license('MIT')

set_languages('c++23')
set_warnings('allextra')
set_policy('package.requires_lock', true)
set_toolset('msvc', 'ninja')

add_rules('mode.debug', 'mode.releasedbg', 'mode.release')

-- Skyrim AE 1.6.1170. Configure once with:
--   xmake f -P . -y -p windows -a x64 -m release --skyrim_ae=y --skyrim_se=n --skyrim_vr=n
target('CombatGroupDeadlockFix')
    add_deps('commonlibsse-ng')
    add_rules('commonlib.plugin')
    on_load(function(target)
        target:data_set('commonlib.plugin.config', {
            name        = 'CombatGroupDeadlockFix',
            author      = 'KezC7',
            description = "Stops the combat group check waiting forever on a combat group lock (vanilla deadlock)."
        })
    end)
    add_files('src/**.cpp')
    add_includedirs('src')
    set_pcxxheader('src/pch.h')
