"""Prepare an isolated build of the upstream Linux and advanced graphics runtime."""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess

from jmafort_backend import BRANDING, OVERLAY, RELEASE_VERSION, ROOT, branded_source, identify_xbe, link_directory, run

UPSTREAM = 'https://github.com/jmafort/CastlevaniaCOD-PC.git'
UPSTREAM_PIN = 'e3d9d83b104f641c5064794156024421da5adcce'
SDK_REPO = 'https://github.com/TGP482/XboxXBE-Recomp.git'
SDK_PIN = '91146ff6e2a218b19c1109dc665d06020472ab57'
WORK = ROOT / 'output' / ('noble-next' if os.name == 'nt' else 'noble-next-linux')
PORT = WORK / 'port'
SDK = WORK / 'sdk'


def checkout(path, url, revision):
    if not (path / '.git').exists():
        path.mkdir(parents=True, exist_ok=True)
        run('git', 'init', path)
    try:
        head = subprocess.check_output(['git', '-C', str(path), 'rev-parse', '--verify', 'HEAD'],
                                       text=True, stderr=subprocess.DEVNULL).strip()
    except subprocess.CalledProcessError:
        run('git', '-C', path, 'fetch', '--depth=1', url, revision)
        run('git', '-C', path, 'checkout', '--detach', 'FETCH_HEAD')
    else:
        if head != revision:
            raise RuntimeError(f'{path} must be at {revision}; found {head}')


def upstream(name):
    return subprocess.check_output(['git', '-C', str(PORT), 'show', f'{UPSTREAM_PIN}:{name}']).decode('utf-8')


def replace(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError(f'Pinned source no longer has exactly one {old!r}')
    return text.replace(old, new)


def write(name, text):
    (PORT / name).write_text(text, encoding='utf-8', newline='\n')


def dlss_supersampling_edits():
    # The 4K/200% preset otherwise asks Streamline to run DLAA at 8K,
    # exhausting VRAM as the first 3D scene creates its render targets.
    return {
        'app/app.h': [('float RenderScale(uint32_t guest_height);',
                       'float RenderScale(uint32_t guest_height, bool dlss = false);')],
        'app/settings.cpp': [(
            'float RenderScale(uint32_t guest_height) {\n'
            '    float h = CachedOutput().height * (Get().render_percent / 100.0f);',
            'float RenderScale(uint32_t guest_height, bool dlss) {\n'
            '    const uint32_t output_height = CachedOutput().height;\n'
            '    float h = output_height * (Get().render_percent / 100.0f);\n'
            '    if (dlss && h > std::max(output_height, 2160u)) {\n'
            '        h = float(std::max(output_height, 2160u));\n'
            '        LOG_FIRST(1, "render scale: limiting DLSS supersampling to %u lines", uint32_t(h));\n'
            '    }')],
        'gpu/d3d12.cpp': [
            ('scale_ = app::RenderScale(guest_h_);',
             'scale_ = app::RenderScale(guest_h_, aa_dlss_start_);'),
            ('float want = app::RenderScale(guest_h_), aspect = TargetAspect();',
             'float want = app::RenderScale(guest_h_, kind == framegen::kAaDlss), aspect = TargetAspect();')],
    }


def windows_controls(info):
    # Keep Noble's direct-look controls on Windows. Linux uses the SDL input
    # path until the mouse capture adapter supports relative mode there.
    for name in ('modern_controls.cpp', 'modern_controls.h', 'modern_camera_math.h'):
        shutil.copy2(OVERLAY / 'src/common' / name, PORT / 'src/common' / name)
    info = '#include "common/modern_controls.h"\n' + info
    info = replace(info, '.load = nullptr,', '.load = modern::LoadSettings,')
    info = replace(info, '.save = nullptr,', '.save = modern::SaveSettings,')
    keyboard = '#include "common/modern_controls.h"\n' + upstream('src/common/keyboard.cpp')
    keyboard = replace(keyboard, '    if (port != 0) return;',
                       '    if (port != 0) return;\n    const bool noble_scripted = !Script().empty() || LiveOn();\n'
                       '    modern::Frame(Focused(), MenuOpen(), noble_scripted, d3d::Swaps());')
    keyboard = replace(keyboard, '    if (g_invert_camera_x) p.rx = Flip(p.rx);\n'
                       '    if (g_invert_camera_y) p.ry = Flip(p.ry);',
                       '    if (!noble_scripted) modern::ApplyPad(p);')
    keyboard = replace(keyboard, '''// a stick under 1/32 of its throw from centre is at rest: 4 of the PS2's 128, a quarter of the game's own dead zone
void Rest(int16_t& x, int16_t& y) {
    constexpr float kRest = 1024;
    if (float(x) * float(x) + float(y) * float(y) < kRest * kRest) x = y = 0;
}

''', '')
    keyboard = replace(keyboard, '''    // the PS2's sticks (hooks.cpp) reach the game as they are: a stick at rest, a few hundred off centre, is centred
    if (g_ps2_stick) {
        Rest(p.lx, p.ly);
        Rest(p.rx, p.ry);
    }''',
                       '    if (!noble_scripted) modern::ApplyStickDeadzones(p);')
    write('src/common/keyboard.cpp', keyboard)
    warp = '#include "common/modern_controls.h"\n' + upstream('src/game/warp.cpp')
    write('src/game/warp.cpp', replace(warp, '    CV_ORIGINAL(CV_CAMERA_UPDATE)(ctx, base);',
                                      '    CV_ORIGINAL(CV_CAMERA_UPDATE)(ctx, base);\n    modern::AfterCameraUpdate(base);'))
    menu = '#include "common/modern_controls.h"\n' + upstream('src/common/menu.cpp')
    menu = replace(menu, 'bool g_scaled = false;',
                   'bool g_scaled = false;\nfloat g_font_scale = 1.0f;\nfloat g_ui_scale = 1.0f;')
    menu = replace(menu, '    Style(scale);\n    g_scaled = true;',
                   '    Style(scale);\n    g_font_scale = g_ui_scale = scale;\n    g_scaled = true;')
    menu = replace(menu, '    PollKeys();\n    ImGuiIO& io = ImGui::GetIO();',
                   '''    PollKeys();
    ImGuiIO& io = ImGui::GetIO();
    // A window on a high-resolution monitor can be much smaller than the desktop.
    // Keep its settings tabs and dropdowns within the actual client area.
    const float ui_scale = std::clamp(io.DisplaySize.y / 1080.0f, 0.75f, g_font_scale);
    if (std::fabs(ui_scale - g_ui_scale) > 0.001f) {
        Style(ui_scale);
        io.FontGlobalScale = ui_scale / g_font_scale;
        g_ui_scale = ui_scale;
    }''')
    menu = replace(menu, '    io.MouseDrawCursor = false;', '''    modern::SetMenuOpen(g_open);
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.MouseDrawCursor = false;
    if (!g_open && modern::MouseCaptured() && GetForegroundWindow() == static_cast<HWND>(app::Window())) {
        auto* overlay = ImGui::GetForegroundDrawList();
        const ImVec2 center{io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f};
        const float radius = std::max(2.0f, io.DisplaySize.y / 540.0f);
        overlay->AddCircleFilled(center, radius + 1.0f, IM_COL32(0, 0, 0, 220));
        overlay->AddCircleFilled(center, radius, IM_COL32(255, 255, 255, 245));
    }''')
    menu = replace(menu, 'uintptr_t wp, intptr_t, intptr_t* result) {', '''uintptr_t wp, intptr_t lp, intptr_t* result) {
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        SetCursor(nullptr);
        *result = TRUE;
        return true;
    }
    if (msg == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) modern::SetMenuOpen(true);''')
    # Use the donor's Controls page and append Noble's camera options.
    menu = replace(menu, 'void ControlsTab(bool& save) {', 'void ControlsTab(bool& save) {\n    save |= modern::DrawSettings();')
    menu = replace(menu, 'void Defaults() {', 'void Defaults() {\n    modern::ResetDefaults();')
    write('src/common/menu.cpp', menu)
    return info


def prepare(assets, eu_assets):
    for region, source in (('usa', assets), ('eu', eu_assets)):
        if source and (identify_xbe(source / 'default.xbe') != region or
                       not all((source / n).is_dir() for n in ('data', 'movie', 'sound'))):
            raise RuntimeError(f'{source}: expected extracted {region.upper()} Xbox game files')
    checkout(PORT, UPSTREAM, UPSTREAM_PIN)
    checkout(SDK, SDK_REPO, SDK_PIN)
    patch = PORT / 'sdk-patches/xboxxbe-recomp.patch'
    if patch.read_text(encoding='utf-8') != upstream('sdk-patches/xboxxbe-recomp.patch'):
        raise RuntimeError('The upstream SDK patch was modified')
    edits = dlss_supersampling_edits() if os.name == 'nt' else {}
    # The donor patch overlaps these hunks. On a rebuild, recognize Noble's
    # postimages and check the donor patch against the rest of the tree.
    excludes = []
    for name, replacements in edits.items():
        source = (SDK / 'runtime' / name).read_text(encoding='utf-8')
        if all(new in source for _, new in replacements):
            excludes.append(f'--exclude=runtime/{name}')
    if subprocess.run(['git', '-C', str(SDK), 'apply', '-R', '--check', *excludes, str(patch)],
                      capture_output=True).returncode:
        run('git', '-C', SDK, 'apply', '--check', patch)
        run('git', '-C', SDK, 'apply', '--whitespace=nowarn', patch)

    for name, replacements in edits.items():
        path = SDK / 'runtime' / name
        source = path.read_text(encoding='utf-8')
        updated = source
        for old, new in replacements:
            if new not in updated:
                updated = replace(updated, old, new)
        if updated != source:
            path.write_text(updated, encoding='utf-8', newline='\n')

    for name in ('config/game.toml', 'config/game_eu.toml'):
        (PORT / name).write_bytes(branded_source(name, upstream(name).encode()))
    cmake = upstream('CMakeLists.txt')
    # Reuse the existing target-name and no-game-icon adaptation.
    cmake = cmake.replace('project(castlevania VERSION 1.9.0', 'project(castlevania VERSION 1.5.1', 1)
    cmake = branded_source('CMakeLists.txt', cmake.encode()).decode()
    cmake = cmake.replace('https://github.com/jmafort/CastlevaniaCOD-PC', 'https://github.com/sergiomanzur/noble-archimedes')
    write('CMakeLists.txt', cmake)
    write('noble-branding.cmake', '''if(WIN32)
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/noble-icon.rc"
        CONTENT "1 ICON \\"@NOBLE_BRANDING_ASSETS@/noble-archimedes.ico\\"\\n" @ONLY)
    foreach(exe ${CV_RELEASES} NobleArchimedesLauncher)
        target_sources(${exe} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/noble-icon.rc")
    endforeach()
    target_link_libraries(NobleArchimedesLauncher PRIVATE winmm)
endif()
add_custom_command(TARGET NobleArchimedesLauncher POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${NOBLE_BRANDING_ASSETS}"
        "$<TARGET_FILE_DIR:NobleArchimedesLauncher>/branding"
    VERBATIM)
''')

    info = branded_source('src/common/game_info.cpp', upstream('src/common/game_info.cpp').encode()).decode()
    info = replace(info, '.icon = GameIcon,', '.icon = nullptr,')
    if os.name == 'nt':
        info = windows_controls(info)
    write('src/common/game_info.cpp', info)
    cvtest = upstream('tools/cvtest/cvtest.py')
    cvtest = re.sub(r'\bCastlevania(EU)?\b', r'NobleArchimedes\1', cvtest)
    cvtest = replace(cvtest, 'kind in ("game", "NobleArchimedes")', 'kind in ("game", "Castlevania")')
    write('tools/cvtest/cvtest.py', cvtest)
    words = upstream('src/launcher/words.h')
    words = words.replace('Castlevania: Curse of Darkness - Launcher', 'Noble Archimedes - Launcher')
    write('src/launcher/words.h', words)
    launcher = upstream('src/launcher/launcher.cpp')
    launcher = re.sub(r'\bCastlevaniaLauncher\b', 'NobleArchimedesLauncher', launcher)
    launcher = re.sub(r'\bCastlevaniaEU\b', 'NobleArchimedesEU', launcher)
    launcher = launcher.replace('"Castlevania.exe"', '"NobleArchimedes.exe"').replace('"Castlevania"', '"NobleArchimedes"')
    launcher = launcher.replace('Castlevania: Curse of Darkness - Launcher', 'Noble Archimedes - Launcher')
    launcher = launcher.replace('updater_.Check(settings_.update_check && !std::getenv("CV_NO_UPDATE_CHECK"));',
                                'updater_.Check(false);')
    # The preview uses the Noble art, independently of any texture pack.
    a = launcher.index('    void LoadArt() {')
    b = launcher.index('\n    }', a) + len('\n    }')
    launcher = launcher[:a] + '''    void LoadArt() {
        if (icon_tried_) return;
        icon_tried_ = true;
        const fs::path art = ExeDir() / "branding";
        crest_ = LoadLogo(art / "noble-icon.png", 256, true);
        background_ = LoadLogo(art / "launcher-background.png", 1920, true);
    }''' + launcher[b:]
    launcher = replace(launcher, 'Picture logo_, crest_;', 'Picture logo_, crest_, background_;')
    launcher = replace(launcher, 'Picture LoadLogo(const fs::path& file, uint32_t max_w) {',
                       'Picture LoadLogo(const fs::path& file, uint32_t max_w, bool original = false) {')
    launcher = replace(launcher, '    UINT x0 = w, y0 = h, x1 = 0, y1 = 0;',
                       '    if (original) return Upload(px, w, h);\n    UINT x0 = w, y0 = h, x1 = 0, y1 = 0;')
    launcher = replace(launcher, '        Backdrop(bg, io.DisplaySize, S(232));', '''        Backdrop(bg, io.DisplaySize, S(232));
        if (background_.view) {
            bg->AddImage(background_.view, {0, 0}, io.DisplaySize, {0, 0}, {1, 1}, IM_COL32(255, 255, 255, 150));
            bg->AddRectFilled({0, 0}, io.DisplaySize, IM_COL32(8, 6, 13, 155));
        }''')
    launcher = launcher.replace('"NobleArchimedes");\n            float tw', '"Noble Archimedes");\n            float tw')
    launcher = launcher.replace('"NobleArchimedes").x', '"Noble Archimedes").x')
    if os.name == 'nt':
        for name in ('launcher.cpp', 'words.h'):
            write('src/launcher/' + name, (OVERLAY / 'src/launcher' / name).read_text(encoding='utf-8'))
    else:
        write('src/launcher/launcher.cpp', launcher)

    for source, dest in ((assets, PORT / 'assets'), (eu_assets, PORT / 'assets/eu')):
        if not source:
            continue
        dest.mkdir(parents=True, exist_ok=True)
        xbe = dest / 'default.xbe'
        if xbe.exists() and xbe.read_bytes() != (source / xbe.name).read_bytes():
            raise RuntimeError(f'Refusing to overwrite {xbe}')
        if not xbe.exists():
            shutil.copy2(source / xbe.name, xbe)
        for name in ('data', 'movie', 'sound'):
            link_directory(source / name, dest / name)
    print(f'Prepared {PORT} with the pinned advanced SDK at {SDK}', flush=True)


def package_linux():
    # Keep the upstream container's glibc baseline and vendor SDK layout.
    script = upstream('tools/linux/release.sh')
    script = script.replace('CastlevaniaCOD-PC', 'NobleArchimedes').replace('Castlevania', 'NobleArchimedes')
    script = replace(script, '-v "$CACHE:/cache"', '-v "$NOBLE_BRANDING_ASSETS:/branding:ro" -v "$CACHE:/cache"')
    script = replace(script, '-DCV_GAME_ICON=OFF', '-DCV_GAME_ICON=OFF -DNOBLE_BRANDING_ASSETS=/branding')
    script = replace(script, 'cp $B/NobleArchimedes $B/NobleArchimedesEU $B/NobleArchimedesLauncher $PKG/',
                     'cp $B/NobleArchimedes $B/NobleArchimedesEU $B/NobleArchimedesLauncher $PKG/\n'
                     'cp -R $B/branding $PKG/branding')
    write('tools/linux/release.sh', script)
    notes = upstream('tools/release/README-linux.txt')
    notes = notes.replace('CastlevaniaCOD-PC', 'Noble Archimedes').replace('CastlevaniaLauncher', 'NobleArchimedesLauncher')
    write('tools/release/README-linux.txt', notes)
    env = os.environ | {'XBE_SDK': str(SDK), 'NOBLE_BRANDING_ASSETS': str(BRANDING),
                        'CV_CACHE': str(WORK / 'package-cache'), 'CV_WORK': str(WORK / 'package-work')}
    subprocess.run(['bash', str(PORT / 'tools/linux/release.sh'), RELEASE_VERSION + '-preview'],
                   cwd=PORT, env=env, check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--assets', type=Path, default=ROOT / 'extracted')
    parser.add_argument('--eu-assets', type=Path)
    action = parser.add_mutually_exclusive_group()
    action.add_argument('--build', action='store_true')
    action.add_argument('--package-linux', action='store_true',
                        help='build a portable Linux preview archive with rootless Podman')
    parser.add_argument('--full-game', action='store_true')
    parser.add_argument('--jobs', type=int, default=4)
    parser.add_argument('--cmake-option', action='append', default=[], metavar='NAME=VALUE')
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.package_linux and (os.name == 'nt' or not args.eu_assets):
        parser.error('--package-linux needs Linux and both extracted Xbox regions')
    try:
        prepare(args.assets.resolve(), args.eu_assets.resolve() if args.eu_assets else None)
        if args.package_linux:
            package_linux()
        if args.build:
            build = WORK / ('build-windows' if os.name == 'nt' else 'build-linux')
            run('cmake', '-S', PORT, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
                '-DCMAKE_C_COMPILER=clang', '-DCMAKE_CXX_COMPILER=clang++',
                '-DCMAKE_C_FLAGS=-march=x86-64-v2', '-DCMAKE_CXX_FLAGS=-march=x86-64-v2',
                f'-DFETCHCONTENT_SOURCE_DIR_XBE_SDK={SDK.as_posix()}', f'-DNOBLE_BRANDING_ASSETS={BRANDING.as_posix()}',
                *(f'-D{x}' for x in args.cmake_option))
            games = ['NobleArchimedes']
            if (PORT / 'assets/eu/default.xbe').is_file():
                games.append('NobleArchimedesEU')
            targets = ['NobleArchimedesLauncher', *games, 'xbe_recomp']
            if args.full_game:
                targets.extend(f'{game}_code' for game in games)
            elif os.name != 'nt':
                targets.extend(f'{game}_hooks' for game in games)
            run('cmake', '--build', build, '--parallel', args.jobs, '--target', *targets)
            link_directory(PORT / 'assets', build / 'assets')
            print(f'Built {build}', flush=True)
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
