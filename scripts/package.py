#!/usr/bin/env python3
"""Package a release build of FluentHMI Studio into a self-contained zip.

The studio exports applications by copying the runtime it ships with, Qt included. The
package carries Qt once: the studio runs on the runtime's copy.

  macOS    FluentHMI Studio.app/Contents/Helpers/FluentHMIRuntime.app holds Qt; the studio's
           rpath and qt.conf point into it
  Windows  FluentHMI-Studio/ holds both executables and one set of Qt DLLs and plugins;
           runtime-files.txt names the ones an exported application takes

After packaging, the script exports examples/demo.uidp with the packaged studio and starts
the exported application with --check, so a release that cannot export, or exports an
application that does not start, fails here instead of on a user's machine.

    python3 scripts/package.py --build-dir build/release --output-dir dist
"""
import argparse
import hashlib
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CHECK_TIMEOUT = 120  # seconds for each run of a packaged program
# Variables that point Qt at the build machine's kit. A user's machine has none of them, and
# Qt searches QT_PLUGIN_PATH (which install-qt-action sets) before the paths in qt.conf.
BUILD_ONLY_ENV = ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH",
                  "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH")


def run(cmd, **kwargs):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], check=True, **kwargs)


def cache_value(build_dir, key):
    cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8", errors="replace")
    match = re.search(rf"^{re.escape(key)}:[A-Z]+=(.*)$", cache, re.MULTILINE)
    return match.group(1).strip() if match else ""


def qt_bin_dir(build_dir):
    """The Qt kit the build used: <prefix>/lib/cmake/Qt6 -> <prefix>/bin."""
    qt_dir = cache_value(build_dir, "Qt6_DIR")
    if not qt_dir:
        sys.exit("Qt6_DIR not found in CMakeCache.txt; is this a configured build directory?")
    prefix = Path(qt_dir).resolve().parent.parent.parent
    for candidate in (prefix / "bin", prefix / "libexec"):
        if candidate.is_dir():
            return candidate
    sys.exit(f"no Qt bin directory under {prefix}")


def find_built(build_dir, name, config):
    """A target's output in a single- or multi-config build tree."""
    for candidate in (build_dir / name, build_dir / config / name):
        if candidate.exists():
            return candidate
    sys.exit(f"{name} not found in {build_dir}; build the {config} configuration first")


def user_env(**extra):
    """The environment to run packaged programs in: what they would see on a user's machine."""
    env = {key: value for key, value in os.environ.items() if key not in BUILD_ONLY_ENV}
    env.update(extra)
    return env


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    out = path.with_name(path.name + ".sha256")
    # LF even on Windows: `shasum -c` reads a CR as part of the file name.
    out.write_text(f"{digest.hexdigest()}  {path.name}\n", encoding="utf-8", newline="\n")
    return out


def check_export(studio_exe, exported_runtime, workdir):
    """Export the demo project with the packaged studio and start the result."""
    out_dir = Path(workdir) / "export-check"
    out_dir.mkdir()
    # Timeouts, so a check that ends up waiting on a window fails instead of hanging.
    run([studio_exe, "--export", out_dir, ROOT / "examples" / "demo.uidp"], timeout=CHECK_TIMEOUT, env=user_env())
    exported = [p for p in out_dir.iterdir() if not p.name.endswith(".partial")]
    if len(exported) != 1:
        sys.exit(f"export produced {exported!r}")
    app = exported[0]
    # The platform's own plugin: deployed applications carry no offscreen plugin, and
    # --check never shows a window.
    run([exported_runtime(app), "--check"], timeout=CHECK_TIMEOUT, env=user_env())
    return app


# ---------------------------------------------------------------------------
# macOS
# ---------------------------------------------------------------------------

def macho_files(bundle):
    """Executables, libraries and plugins in a bundle.

    By content: framework binaries are neither executable nor named .dylib.
    """
    for path in bundle.rglob("*"):
        if path.is_symlink() or not path.is_file():
            continue
        with open(path, "rb") as stream:
            magic = stream.read(4)
        if magic in (b"\xcf\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xce\xfa\xed\xfe"):
            yield path


def otool(*args):
    return subprocess.run(["otool", *map(str, args)], capture_output=True, text=True).stdout


# otool lists a universal binary once per architecture, each under a "<file> (architecture
# x):" header, while one install_name_tool call changes every architecture. So the helpers
# drop the headers and return each name once; deleting an rpath twice is an error.

def macho_install_name(path):
    lines = [line.strip() for line in otool("-D", path).splitlines()[1:] if not line.endswith(":")]
    return lines[0] if lines else ""


def macho_dependencies(path):
    own = macho_install_name(path)
    deps = [line.strip().split(" (compatibility")[0] for line in otool("-L", path).splitlines()[1:]
            if not line.endswith(":")]
    return list(dict.fromkeys(dep for dep in deps if dep and dep != own))


def macho_rpaths(path):
    lines = otool("-l", path).splitlines()
    return list(dict.fromkeys(lines[i + 2].split()[1] for i, line in enumerate(lines)
                              if line.strip() == "cmd LC_RPATH" and i + 2 < len(lines)))


def fix_macos_load_paths(bundle):
    """Point the bundle at its own Frameworks only, and only through @rpath.

    CMake leaves the build machine's library directory (/opt/homebrew/lib for Homebrew's Qt)
    as the executables' only rpath, and macdeployqt keeps it. Libraries that load each other
    through @rpath then still resolve on the build machine and fail everywhere else.

    macdeployqt also writes some references as @executable_path/../Frameworks/..., which
    only holds while this bundle's own executable is the one running. The studio runs on
    the runtime's libraries (share_runtime_qt), so every reference goes through @rpath and
    each executable's rpath says where its Frameworks are.
    """
    frameworks = "@executable_path/../Frameworks"
    for path in macho_files(bundle):
        for rpath in macho_rpaths(path):
            if not rpath.startswith("@"):
                run(["install_name_tool", "-delete_rpath", rpath, path])
        in_macos = path.parent.name == "MacOS"
        if in_macos and frameworks not in macho_rpaths(path):
            run(["install_name_tool", "-add_rpath", frameworks, path])
        own = macho_install_name(path)
        changes = []
        if own.startswith("/") and path.parent.name == "Frameworks":
            changes += ["-id", f"@rpath/{path.name}"]
        elif own.startswith(frameworks + "/"):
            changes += ["-id", "@rpath/" + own[len(frameworks) + 1:]]
        for dep in macho_dependencies(path):
            if dep.startswith(frameworks + "/"):
                changes += ["-change", dep, "@rpath/" + dep[len(frameworks) + 1:]]
        if changes:
            run(["install_name_tool", *changes, path])


def share_runtime_qt(studio, runtime):
    """Run the studio on the Qt inside its runtime instead of a second copy of its own.

    The runtime has to keep its own Qt, since exported applications are copies of it. Every
    framework and plugin macdeployqt gave the studio must be in the runtime as well; the
    studio's copies are then removed, its rpath points at the runtime's Frameworks and its
    qt.conf at the runtime's PlugIns.
    """
    for folder in ("Frameworks", "PlugIns"):
        own = studio / "Contents" / folder
        if not own.is_dir():
            continue
        shared = runtime / "Contents" / folder
        missing = sorted(str(p.relative_to(own)) for p in own.rglob("*")
                         if p.is_file() and not (shared / p.relative_to(own)).exists())
        if missing:
            sys.exit(f"the studio needs {folder} the runtime lacks:\n  " + "\n  ".join(missing))
        shutil.rmtree(own)

    inside = runtime.relative_to(studio / "Contents")
    for path in macho_files(studio / "Contents" / "MacOS"):
        for rpath in macho_rpaths(path):
            run(["install_name_tool", "-delete_rpath", rpath, path])
        run(["install_name_tool", "-add_rpath", f"@executable_path/../{inside}/Contents/Frameworks", path])
    # The prefix qt.conf paths are relative to is the bundle's Contents.
    (studio / "Contents" / "Resources" / "qt.conf").write_text(
        f"[Paths]\nPlugins = {inside}/Contents/PlugIns\n", encoding="utf-8")


def macos_loaded_outside(cmd, bundle):
    """Images a command loads from anywhere but the system and the given bundle.

    The static checks only see load commands; this also catches a qt.conf that misses and
    lets Qt fall back to the plugins of the Qt it was built with.
    """
    print("+", " ".join(str(c) for c in cmd), flush=True)
    result = subprocess.run([str(c) for c in cmd], env=user_env(DYLD_PRINT_LIBRARIES="1"), capture_output=True,
                            text=True, timeout=CHECK_TIMEOUT)
    if result.returncode != 0:
        sys.exit(f"exit {result.returncode}:\n{result.stdout}{result.stderr}")
    root = os.path.realpath(bundle) + os.sep
    loaded = re.findall(r"^dyld\[\d+\]: (?:<[^>]*> )?(/.+)$", result.stderr, re.MULTILINE)
    if not loaded:
        sys.exit("DYLD_PRINT_LIBRARIES printed nothing; cannot tell where libraries load from")
    return [image for image in loaded
            if not image.startswith(("/System/", "/usr/lib/")) and not os.path.realpath(image).startswith(root)]


def macos_linked_outside(bundle, forbidden_prefix):
    """Binaries that would still load something from the build machine's Qt prefix."""
    offenders = []
    for path in macho_files(bundle):
        outside = [dep for dep in macho_dependencies(path) if dep.startswith(forbidden_prefix)]
        outside += [rpath for rpath in macho_rpaths(path) if not rpath.startswith("@")]
        if outside:
            offenders.append(f"{path.relative_to(bundle)}: {', '.join(outside)}")
    return offenders


def package_macos(build_dir, output_dir, version, config):
    qt_bin = qt_bin_dir(build_dir)
    deploy = qt_bin / "macdeployqt"
    qt_prefix = str(qt_bin.parent)
    stage = output_dir / "stage"
    shutil.rmtree(stage, ignore_errors=True)
    stage.mkdir(parents=True)

    runtime = stage / "FluentHMIRuntime.app"
    shutil.copytree(find_built(build_dir, "FluentHMIRuntime.app", config), runtime, symlinks=True)
    run([deploy, runtime, "-always-overwrite", "-verbose=1"])
    fix_macos_load_paths(runtime)
    # Ad hoc, so Apple Silicon runs it; a Developer ID signature is a separate, later step.
    run(["codesign", "--force", "--deep", "--sign", "-", runtime])

    studio = stage / "FluentHMI Studio.app"
    shutil.copytree(find_built(build_dir, "FluentHMIStudio.app", config), studio, symlinks=True)
    run([deploy, studio, "-always-overwrite", "-verbose=1"])
    fix_macos_load_paths(studio)
    # Helpers is where nested applications belong; signing and notarization look there.
    (studio / "Contents" / "Helpers").mkdir()
    runtime = Path(shutil.move(str(runtime), str(studio / "Contents" / "Helpers" / "FluentHMIRuntime.app")))
    share_runtime_qt(studio, runtime)
    shutil.copytree(ROOT / "examples", studio / "Contents" / "Resources" / "examples")

    offenders = macos_linked_outside(studio, qt_prefix)
    if offenders:
        sys.exit("still linked against the build machine's Qt:\n  " + "\n  ".join(offenders))
    run(["codesign", "--force", "--deep", "--sign", "-", studio])
    run(["codesign", "--verify", "--deep", "--strict", studio])
    run([runtime / "Contents" / "MacOS" / "FluentHMIRuntime", "--check", ROOT / "examples" / "demo.uidp"],
        timeout=CHECK_TIMEOUT, env=user_env())

    studio_exe = studio / "Contents" / "MacOS" / "FluentHMIStudio"
    with tempfile.TemporaryDirectory() as work:
        app = check_export(studio_exe, lambda exported: exported / "Contents" / "MacOS" / "FluentHMIRuntime", work)
        offenders = macos_linked_outside(app, qt_prefix)
        if offenders:
            sys.exit("the exported application still uses the build machine's Qt")
        (Path(work) / "dyld-check").mkdir()
        for cmd, bundle in (([studio_exe, "--export", Path(work) / "dyld-check", ROOT / "examples" / "demo.uidp"], studio),
                            ([app / "Contents" / "MacOS" / "FluentHMIRuntime", "--check"], app)):
            outside = macos_loaded_outside(cmd, bundle)
            if outside:
                sys.exit(f"{bundle.name} loads libraries from outside itself:\n  " + "\n  ".join(outside))

    arch = "universal" if "x86_64" in cache_value(build_dir, "CMAKE_OSX_ARCHITECTURES") and \
        "arm64" in cache_value(build_dir, "CMAKE_OSX_ARCHITECTURES") else platform.machine()
    archive = output_dir / f"FluentHMI-Studio-v{version}-macos-{arch}.zip"
    archive.unlink(missing_ok=True)
    # ditto keeps the framework symbolic links and permissions zipfile would lose.
    run(["ditto", "-c", "-k", "--sequesterRsrc", "--keepParent", studio, archive])
    return archive


# ---------------------------------------------------------------------------
# Windows
# ---------------------------------------------------------------------------

def vc_runtime_dlls():
    """The Visual C++ runtime DLLs to ship beside the executables.

    windeployqt only deploys them from a developer prompt (VCINSTALLDIR), and a zip has no
    installer to put the redistributable on a PC that lacks it.
    """
    roots = []
    if os.environ.get("VCToolsRedistDir"):
        roots.append(Path(os.environ["VCToolsRedistDir"]))
    vswhere = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / \
        "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
    if vswhere.exists():
        found = subprocess.run([str(vswhere), "-latest", "-products", "*", "-property", "installationPath"],
                               capture_output=True, text=True).stdout.strip()
        if found:
            roots += sorted((Path(found) / "VC" / "Redist" / "MSVC").glob("*"), reverse=True)
    for root in roots:
        crt = sorted((root / "x64").glob("Microsoft.VC*.CRT"), reverse=True)
        if crt:
            dlls = sorted(crt[0].glob("*.dll"))
            if dlls:
                return dlls
    sys.exit("Visual C++ runtime not found: run from a Visual Studio developer prompt "
             "or install the Visual Studio C++ build tools")


def package_windows(build_dir, output_dir, version, config):
    qt_bin = qt_bin_dir(build_dir)
    deploy = qt_bin / "windeployqt.exe"
    name = f"FluentHMI-Studio-v{version}-windows-x64"
    stage = output_dir / "stage" / name
    shutil.rmtree(stage.parent, ignore_errors=True)
    stage.mkdir(parents=True)

    # --no-compiler-runtime: the DLLs are bundled below; from a developer prompt windeployqt
    # would add the vc_redist installer instead.
    deploy_args = ["--release" if config.lower() != "debug" else "--debug", "--no-translations",
                   "--no-system-d3d-compiler", "--no-opengl-sw", "--no-compiler-runtime"]
    # The runtime goes into the empty folder first, so what windeployqt lists is exactly what
    # it needs. runtime-files.txt keeps that list: an exported application copies those files
    # and leaves the studio's own behind.
    shutil.copy2(find_built(build_dir, "FluentHMIRuntime.exe", config), stage)
    listed = run([deploy, *deploy_args, "--list", "relative", stage / "FluentHMIRuntime.exe"],
                 capture_output=True, text=True).stdout
    runtime_files = ["FluentHMIRuntime.exe"] + [line.strip().replace("\\", "/") for line in listed.splitlines()
                                                if line.strip()]
    shutil.copy2(find_built(build_dir, "FluentHMIStudio.exe", config), stage)
    run([deploy, *deploy_args, stage / "FluentHMIStudio.exe"])
    for dll in vc_runtime_dlls():
        print(f"bundling {dll.name}")
        shutil.copy2(dll, stage)
        runtime_files.append(dll.name)
    unknown = [f for f in runtime_files if not (stage / f).is_file()]
    if unknown:
        sys.exit("windeployqt listed files that are not in the package:\n  " + "\n  ".join(unknown))
    print(f"the runtime takes {len(runtime_files)} files")
    (stage / "runtime-files.txt").write_text("\n".join(runtime_files) + "\n", encoding="utf-8", newline="\n")
    shutil.copytree(ROOT / "examples", stage / "examples")
    shutil.copy2(ROOT / "LICENSE", stage / "LICENSE.txt")

    # The virus scanner can still hold a just-loaded DLL when the folder is removed.
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as work:
        check_export(stage / "FluentHMIStudio.exe",
                     lambda exported: exported / f"{exported.name}.exe", work)

    archive = output_dir / f"{name}.zip"
    archive.unlink(missing_ok=True)
    shutil.make_archive(str(archive.with_suffix("")), "zip", stage.parent, name)
    return archive


def main():
    # Paths here can hold Chinese application names; a Windows console's code page cannot.
    for stream in (sys.stdout, sys.stderr):
        stream.reconfigure(encoding="utf-8", errors="replace")
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", required=True, type=Path)
    parser.add_argument("--output-dir", default=ROOT / "dist", type=Path)
    parser.add_argument("--config", default="Release", help="configuration of a multi-config build")
    parser.add_argument("--version", help="defaults to the CMake project version")
    args = parser.parse_args()

    build_dir = args.build_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    version = args.version or (build_dir / "FluentHMIStudio-version.txt").read_text().strip()

    if sys.platform == "darwin":
        archive = package_macos(build_dir, output_dir, version, args.config)
    elif sys.platform == "win32":
        archive = package_windows(build_dir, output_dir, version, args.config)
    else:
        sys.exit("packaging is provided for macOS and Windows")
    checksum = sha256_file(archive)
    shutil.rmtree(output_dir / "stage", ignore_errors=True)
    print(f"packaged {archive}\nchecksum {checksum}")


if __name__ == "__main__":
    main()
