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
WORK = Path(os.environ['NOBLE_NEXT_WORK']) if os.environ.get('NOBLE_NEXT_WORK') else ROOT / 'output' / ('noble-next' if os.name == 'nt' else 'noble-next-linux')
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


def amd_gcn_vertex_stride_edits():
    # Ported from jmafort/CastlevaniaCOD-PC ccd5000 (issue #16):
    # Castlevania's 2D draws declare an unused second texcoord at byte 28 of 28-byte vertices.
    # AMD GCN GPUs (RX 400/500, Vega) corrupt adjacent attributes when any element reaches past
    # StrideInBytes, smearing quads into vertical streaks. Widen the uploaded stride by tail so
    # every declared element stays strictly inside its vertex record.
    return {
        'gpu/vertex.h': [(
            'struct VertexBuffer {\n'
            '    const uint8_t* src = nullptr;\n'
            '    uint32_t bytes = 0, stride = 0;\n'
            '};',
            'struct VertexBuffer {\n'
            '    const uint8_t* src = nullptr;\n'
            '    uint32_t bytes = 0, stride = 0;\n'
            '    uint32_t tail = 0;\n'
            '};')],
        'gpu/vertex.cpp': [(
            '        out.elements[size_t(reg)] = {he.format, uint8_t(slot[in.stream]), uint16_t(in.offset)};\n'
            '        out.decode |= uint64_t(he.decode) << (reg * 4);\n'
            '    }',
            '        out.elements[size_t(reg)] = {he.format, uint8_t(slot[in.stream]), uint16_t(in.offset)};\n'
            '        out.decode |= uint64_t(he.decode) << (reg * 4);\n'
            '        VertexBuffer& b = out.buffers[size_t(slot[in.stream])];\n'
            '        if (in.offset + he.bytes > b.stride) b.tail = std::max(b.tail, in.offset + he.bytes - b.stride);\n'
            '    }')],
        'gpu/d3d12.cpp': [(
            'void D3D12Backend::UploadVertexBuffers(const Assembled& a) {\n'
            '    D3D12_VERTEX_BUFFER_VIEW vbv[16];\n'
            '    for (uint32_t i = 0; i < a.buffer_count; i++) {\n'
            '        const VertexBuffer& b = a.buffers[i];\n'
            '        Alloc vb = Upload(b.bytes, 16);\n'
            '        std::memcpy(vb.cpu, b.src, b.bytes);\n'
            '        vbv[i] = {vb.gpu, b.bytes, b.stride};\n'
            '    }\n'
            '    cl_->IASetVertexBuffers(0, a.buffer_count, vbv);\n'
            '}',
            'void D3D12Backend::UploadVertexBuffers(const Assembled& a) {\n'
            '    D3D12_VERTEX_BUFFER_VIEW vbv[16];\n'
            '    for (uint32_t i = 0; i < a.buffer_count; i++) {\n'
            '        const VertexBuffer& b = a.buffers[i];\n'
            '        uint32_t n = b.stride ? b.bytes / b.stride : 0;\n'
            '        if (b.tail && n) {\n'
            '            uint32_t wide = b.stride + b.tail;\n'
            '            Alloc vb = Upload(size_t(n) * wide, 16);\n'
            '            for (uint32_t v = 0; v < n; v++) {\n'
            '                uint8_t* dst = vb.cpu + size_t(v) * wide;\n'
            '                size_t at = size_t(v) * b.stride, have = std::min<size_t>(wide, b.bytes - at);\n'
            '                std::memcpy(dst, b.src + at, have);\n'
            '                if (have < wide) std::memset(dst + have, 0, wide - have);\n'
            '            }\n'
            '            vbv[i] = {vb.gpu, n * wide, wide};\n'
            '            continue;\n'
            '        }\n'
            '        Alloc vb = Upload(b.bytes + b.tail, 16);\n'
            '        std::memcpy(vb.cpu, b.src, b.bytes);\n'
            '        if (b.tail) std::memset(vb.cpu + b.bytes, 0, b.tail);\n'
            '        vbv[i] = {vb.gpu, b.bytes + b.tail, b.stride};\n'
            '    }\n'
            '    cl_->IASetVertexBuffers(0, a.buffer_count, vbv);\n'
            '}')],
    }


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
    edits = amd_gcn_vertex_stride_edits()
    if os.name == 'nt':
        for name, replacements in dlss_supersampling_edits().items():
            edits.setdefault(name, []).extend(replacements)
    # The donor patch overlaps these hunks. On a rebuild, recognize Noble's
    # postimages and check the donor patch against the rest of the tree.
    excludes = []
    for name, replacements in edits.items():
        source = (SDK / 'runtime' / name).read_text(encoding='utf-8')
        if any(new in source for _, new in replacements):
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
    launcher = replace(launcher,
                       '    std::error_code ec;\n'
                       '    return fs::read_symlink("/proc/self/exe", ec).parent_path();',
                       '    // Cache on first call: on Linux /proc/self/exe follows renames, so after\n'
                       '    // PutInPlace moves the running launcher into update/previous/, re-reading\n'
                       '    // it would point Restart() at the old executable instead of the updated one.\n'
                       '    static const fs::path dir = [] {\n'
                       '        std::error_code ec;\n'
                       '        return fs::read_symlink("/proc/self/exe", ec).parent_path();\n'
                       '    }();\n'
                       '    return dir;')
    launcher = replace(launcher,
                       'constexpr const char* kUpdateRepo = "jmafort/CastlevaniaCOD-PC";\n'
                       'constexpr const char* kUpdateApi = "https://api.github.com/repos/jmafort/CastlevaniaCOD-PC/releases?per_page=10";\n'
                       'constexpr const char* kUpdateReleases = "https://github.com/jmafort/CastlevaniaCOD-PC/releases";',
                       'constexpr const char* kUpdateRepo = "sergiomanzur/noble-archimedes";\n'
                       'constexpr const char* kUpdateApi = "https://api.github.com/repos/sergiomanzur/noble-archimedes/releases?per_page=10";\n'
                       'constexpr const char* kUpdateReleases = "https://github.com/sergiomanzur/noble-archimedes/releases";')
    launcher = replace(launcher,
                       'bool OwnPackage(const std::string& name) {\n'
                       '    if (name.compare(0, 17, "CastlevaniaCOD-PC") != 0) return false;\n'
                       '#ifdef _WIN32\n'
                       '    return name.size() > 4 && name.compare(name.size() - 4, 4, ".zip") == 0 && name.find("linux") == std::string::npos;\n'
                       '#else\n'
                       '    constexpr std::string_view kEnd = "-linux.tar.xz";\n'
                       '    return name.size() > kEnd.size() && name.compare(name.size() - kEnd.size(), kEnd.size(), kEnd) == 0;\n'
                       '#endif\n'
                       '}',
                       'bool OwnPackage(const std::string& name) {\n'
                       '    std::string low = name;\n'
                       '    for (char& c : low) c = char(std::tolower(uint8_t(c)));\n'
                       '    if (low.compare(0, 17, "noble-archimedes-") != 0) return false;\n'
                       '#ifdef _WIN32\n'
                       '    constexpr std::string_view kWin = "-windows-x64.zip";\n'
                       '    return low.size() > kWin.size() && low.compare(low.size() - kWin.size(), kWin.size(), kWin) == 0;\n'
                       '#else\n'
                       '    constexpr std::string_view kApp = "-linux-x86_64.appimage";\n'
                       '    constexpr std::string_view kTar = "-linux.tar.xz";\n'
                       '    return (low.size() > kApp.size() && low.compare(low.size() - kApp.size(), kApp.size(), kApp) == 0) ||\n'
                       '           (low.size() > kTar.size() && low.compare(low.size() - kTar.size(), kTar.size(), kTar) == 0);\n'
                       '#endif\n'
                       '}')
    launcher = replace(launcher,
                       'UpdateState LookUpLatest(const std::string& own, PortRelease& out) {\n'
                       '    std::error_code ec;\n'
                       '    fs::path dir = ExeDir() / "update";\n'
                       '    fs::create_directories(dir, ec);\n'
                       '    fs::path list = dir / "releases.json";\n'
                       '    HttpAnswer a = HttpRequest(kUpdateApi, &list, true);\n'
                       '    std::string text;\n'
                       '    {\n'
                       '        std::ifstream f(list, std::ios::binary);\n'
                       '        text.assign(std::istreambuf_iterator<char>(f), {});\n'
                       '    }\n'
                       '    fs::remove(list, ec);\n'
                       '    if (GitHubBusy(a.status)) return UpdateState::Busy;\n'
                       '    if (a.status != 200 || !NewestRelease(text, out)) return UpdateState::Unreachable;\n'
                       '    return out.tag == own ? UpdateState::UpToDate : UpdateState::Available;\n'
                       '}',
                       'int CompareReleaseVersions(std::string a, std::string b) {\n'
                       '    auto parse = [](std::string s, std::vector<uint32_t>& nums, std::string& suffix) -> bool {\n'
                       '        while (!s.empty() && (s.front() == \' \' || s.front() == \'\\t\')) s.erase(s.begin());\n'
                       '        while (!s.empty() && (s.back() == \' \' || s.back() == \'\\t\')) s.pop_back();\n'
                       '        if (!s.empty() && (s[0] == \'v\' || s[0] == \'V\')) s.erase(s.begin());\n'
                       '        size_t i = 0;\n'
                       '        while (i < s.size() && std::isdigit(uint8_t(s[i]))) {\n'
                       '            uint32_t n = 0;\n'
                       '            while (i < s.size() && std::isdigit(uint8_t(s[i]))) {\n'
                       '                if (n < 1000000) n = n * 10 + uint32_t(s[i] - \'0\');\n'
                       '                i++;\n'
                       '            }\n'
                       '            nums.push_back(n);\n'
                       '            if (i < s.size() && s[i] == \'.\') i++;\n'
                       '            else break;\n'
                       '        }\n'
                       '        if (nums.empty()) return false;\n'
                       '        suffix = s.substr(i);\n'
                       '        for (char& c : suffix) c = char(std::tolower(uint8_t(c)));\n'
                       '        return true;\n'
                       '    };\n'
                       '    std::vector<uint32_t> xa, xb;\n'
                       '    std::string sa, sb;\n'
                       '    if (!parse(a, xa, sa) || !parse(b, xb, sb)) return a == b ? 0 : (a < b ? -1 : 1);\n'
                       '    size_t n = (std::max)(xa.size(), xb.size());\n'
                       '    xa.resize(n, 0); xb.resize(n, 0);\n'
                       '    for (size_t i = 0; i < n; i++) if (xa[i] != xb[i]) return xa[i] > xb[i] ? 1 : -1;\n'
                       '    if (sa == sb) return 0;\n'
                       '    if (sa.empty()) return 1;\n'
                       '    if (sb.empty()) return -1;\n'
                       '    return sa > sb ? 1 : -1;\n'
                       '}\n\n'
                       'UpdateState LookUpLatest(const std::string& own, PortRelease& out) {\n'
                       '    std::error_code ec;\n'
                       '    fs::path dir = ExeDir() / "update";\n'
                       '    fs::create_directories(dir, ec);\n'
                       '    fs::path list = dir / "releases.json";\n'
                       '    const char* api = std::getenv("NOBLE_UPDATE_API");\n'
                       '    HttpAnswer a = HttpRequest(api && *api ? std::string(api) : std::string(kUpdateApi), &list, true);\n'
                       '    std::string text;\n'
                       '    {\n'
                       '        std::ifstream f(list, std::ios::binary);\n'
                       '        text.assign(std::istreambuf_iterator<char>(f), {});\n'
                       '    }\n'
                       '    fs::remove(list, ec);\n'
                       '    if (GitHubBusy(a.status)) return UpdateState::Busy;\n'
                       '    if (a.status != 200 || !NewestRelease(text, out)) return UpdateState::Unreachable;\n'
                       '    return CompareReleaseVersions(out.tag, own) > 0 ? UpdateState::Available : UpdateState::UpToDate;\n'
                       '}')
    launcher = replace(launcher,
                       '    static const char* const kKeep[] = {"assets", "textures", "hdd", "logs", "streamline", "licences",\n'
                       '                                        "castlevania.toml", "update", "backup"};',
                       '    static const char* const kKeep[] = {"assets", "textures", "hdd", "logs", "streamline", "licences",\n'
                       '                                        "movies", "mods", "saves", "castlevania.toml", "update", "backup",\n'
                       '                                        "apprun", ".diricon", "noble-archimedes.desktop", "noble-archimedes.png"};')
    launcher = replace(launcher,
                       '        if (!e.is_regular_file(ec)) continue;',
                       '        if (e.is_symlink(ec) || !e.is_regular_file(ec)) continue;')
    launcher = replace(launcher,
                       '    std::ofstream v(game / kVersionFile);\n'
                       '    v << tag << "\\n";\n'
                       '    return 0;',
                       '#ifndef _WIN32\n'
                       '    for (const char* exe : {"NobleArchimedes", "NobleArchimedesEU", "NobleArchimedesLauncher",\n'
                       '                            "compiler/xbe_recomp", "compiler/clang", "compiler/ld.lld"}) {\n'
                       '        fs::path p = game / exe;\n'
                       '        if (fs::exists(p, ec))\n'
                       '            fs::permissions(p, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,\n'
                       '                            fs::perm_options::add, ec);\n'
                       '    }\n'
                       '#endif\n'
                       '    std::ofstream v(game / kVersionFile);\n'
                       '    v << tag << "\\n";\n'
                       '    return 0;')
    launcher = replace(launcher,
                       '            tool == 0 ? std::vector<std::string>{"curl", "-s", "--connect-timeout", "30", "--max-time", "300", "-D",\n'
                       '                                                 hdr.string(), "-o", body}\n'
                       '                      : std::vector<std::string>{"wget", "-S", "-t", "1", "-T", "30", "-o", hdr.string(), "-O", body,\n'
                       '                                                 follow ? "--max-redirect=20" : "--max-redirect=0"};',
                       '            tool == 0 ? std::vector<std::string>{"curl", "-s", "-A", "NobleArchimedesLauncher", "-H",\n'
                       '                                                 "Accept: application/vnd.github+json", "--connect-timeout", "30",\n'
                       '                                                 "--max-time", "300", "-D", hdr.string(), "-o", body}\n'
                       '                      : std::vector<std::string>{"wget", "-S", "-U", "NobleArchimedesLauncher",\n'
                       '                                                 "--header=Accept: application/vnd.github+json", "-t", "1", "-T", "30",\n'
                       '                                                 "-o", hdr.string(), "-O", body,\n'
                       '                                                 follow ? "--max-redirect=20" : "--max-redirect=0"};')
    launcher = replace(launcher,
                       '                code = RunTool({"tar", "-xJf", archive.string(), "-C", unpacked.string()});\n'
                       '                ok = code == 0;',
                       '                std::string ext = archive.extension().string();\n'
                       '                for (char& c : ext) c = char(std::tolower(uint8_t(c)));\n'
                       '                if (ext == ".appimage") {\n'
                       '                    fs::permissions(archive, fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,\n'
                       '                                    fs::perm_options::add, ec);\n'
                       '                    code = RunTool({"sh", "-c", "cd \\"$1\\" && \\"$2\\" --appimage-extract >/dev/null 2>&1", "--",\n'
                       '                                    unpacked.string(), fs::absolute(archive, ec).string()});\n'
                       '                } else {\n'
                       '                    code = RunTool({"tar", "-xJf", archive.string(), "-C", unpacked.string()});\n'
                       '                }\n'
                       '                ok = code == 0;')
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
