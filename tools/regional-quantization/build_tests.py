"""Build isolated map, NVENC probe and worker/bridge tests for this package."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess


def msvc_environment():
    finder = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    install = subprocess.check_output([str(finder), '-latest', '-products', '*', '-requires',
                                      'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                      '-property', 'installationPath'], text=True).strip()
    vcvars = Path(install) / 'VC/Auxiliary/Build/vcvars64.bat'
    # Only obtain the compiler environment here; file operations use pathlib.
    result = subprocess.check_output(f'cmd.exe /d /s /c ""{vcvars}" >nul && set"', text=True)
    env = {key.upper(): value for key, value in os.environ.items()}
    env.update((key.upper(), value) for key, value in
               (line.split('=', 1) for line in result.splitlines() if '=' in line and not line.startswith('=')))
    return env


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    parser.add_argument('--build-only', action='store_true', help='Compile helpers without running the isolated IPC tests')
    parser.add_argument('--nvenc-include', type=Path, help='Header directory for optional hardware output-statistics probes')
    args = parser.parse_args()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    tools = Path(__file__).resolve().parent
    root = tools.parents[1]
    platform = root / 'alvr/server_openvr/cpp/platform/win32'
    producer = tools / 'producer'
    include = root / 'alvr/server_openvr/cpp/alvr_server'
    if args.nvenc_include and not (args.nvenc_include / 'nvEncodeAPI.h').is_file():
        raise ValueError('NVENC header directory is missing nvEncodeAPI.h')
    env = msvc_environment()
    for name in ('map_tests', 'nvenc_probe', 'worker_bridge_tests', 'regional_quality_tests', 'reference_capture_tests', 'dynamic_reference_tests', 'encoder_telemetry_tests'):
        options = ['/nologo', '/std:c++17', '/EHsc', '/O2', '/DNOMINMAX',
                   f'/I{platform}', f'/I{producer}', f'/I{include}',
                   f'/Fe:{output / (name + ".exe")}', f'/Fo:{output}{os.sep}']
        if name == 'nvenc_probe' and args.nvenc_include:
            options.insert(5, f'/I{args.nvenc_include.resolve()}')
        sources = [str(tools / 'tests' / (name + '.cpp'))]
        if name == 'worker_bridge_tests':
            options.append('/DFBA_TESTING')
            sources.extend([str(producer / 'AttentionWorker.cpp'), str(platform / 'AttentionBridge.cpp')])
        command = [shutil.which('cl.exe', path=env['PATH'])] + options + sources
        if name in ('nvenc_probe', 'reference_capture_tests', 'dynamic_reference_tests'):
            command.extend(['/link', 'd3d11.lib', 'dxgi.lib'])
        subprocess.run(command, env=env, cwd=output, check=True)
    # The NVENC probe is deliberately not run automatically: it is a separate GPU experiment.
    if args.build_only:
        return
    subprocess.run([str(output / 'map_tests.exe')], check=True)
    subprocess.run([str(output / 'worker_bridge_tests.exe')], check=True)
    subprocess.run([str(output / 'regional_quality_tests.exe')], check=True)
    subprocess.run([str(output / 'reference_capture_tests.exe')], check=True)
    subprocess.run([str(output / 'dynamic_reference_tests.exe')], check=True)
    subprocess.run([str(output / 'encoder_telemetry_tests.exe')], check=True)


if __name__ == '__main__':
    main()
