#!/usr/bin/env python3
"""Package a release build of FluentHMI Studio into a self-contained zip, or on Linux a .deb.

The studio exports applications by copying the runtime it ships with, Qt included. The
package carries Qt once: the studio runs on the runtime's copy.

  macOS    FluentHMI Studio.app/Contents/Helpers/FluentHMIRuntime.app holds Qt; the studio's
           rpath and qt.conf point into it
  Windows  FluentHMI-Studio/ holds both executables and one set of Qt DLLs and plugins;
           runtime-files.txt names the ones an exported application takes
  Linux    /opt/FluentHMIStudio/ as on Windows, with Qt's libraries in lib/ and its plugins
           in plugins/, which the executables find through their RUNPATH; the .deb adds a
           launcher in /usr/bin, a desktop entry and an icon

After packaging, the script exports examples/demo.uidp with the packaged studio and starts
the exported application with --check, so a release that cannot export, or exports an
application that does not start, fails here instead of on a user's machine. On Linux this
needs a display: run it under xvfb-run -a, or in a desktop session.

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
# Qt searches QT_PLUGIN_PATH (which install-qt-action sets) before the paths in qt.conf. On
# Linux install-qt-action also puts the kit's lib directory on LD_LIBRARY_PATH, which the
# loader searches before a RUNPATH.
BUILD_ONLY_ENV = ("QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH", "QML2_IMPORT_PATH", "QML_IMPORT_PATH",
                  "DYLD_LIBRARY_PATH", "DYLD_FRAMEWORK_PATH", "LD_LIBRARY_PATH")


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


def run_packaged(cmd):
    """Run a packaged program as a user's machine would."""
    # A timeout, so a check that ends up waiting on a window fails instead of hanging.
    run(cmd, timeout=CHECK_TIMEOUT, env=user_env())


def check_export(studio_exe, exported_runtime, workdir, launch=run_packaged):
    """Export the demo project with the packaged studio and start the result, each with launch(cmd)."""
    out_dir = Path(workdir) / "export-check"
    out_dir.mkdir()
    launch([studio_exe, "--export", out_dir, ROOT / "examples" / "demo.uidp"])
    exported = [p for p in out_dir.iterdir() if not p.name.endswith(".partial")]
    if len(exported) != 1:
        sys.exit(f"export produced {exported!r}")
    # The platform's own plugin: deployed applications carry no offscreen plugin, and
    # --check never shows a window.
    launch([exported_runtime(exported[0]), "--check"])


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


def macos_run_inside(cmd):
    """Run a packaged program, failing if it loads anything from outside its own bundle."""
    bundle = Path(cmd[0]).parents[2]  # <bundle>.app/Contents/MacOS/<program>
    outside = macos_loaded_outside(cmd, bundle)
    if outside:
        sys.exit(f"{bundle.name} loads libraries from outside itself:\n  " + "\n  ".join(outside))


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

    # The exported application is a copy of Helpers/FluentHMIRuntime.app, whose files the scan above covered.
    with tempfile.TemporaryDirectory() as work:
        check_export(studio / "Contents" / "MacOS" / "FluentHMIStudio",
                     lambda exported: exported / "Contents" / "MacOS" / "FluentHMIRuntime", work, macos_run_inside)

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


# ---------------------------------------------------------------------------
# Linux
# ---------------------------------------------------------------------------

DEB_PACKAGE = "fluenthmi-studio"
INSTALL_ROOT = "/opt/FluentHMIStudio"  # where UpdateInstaller expects an installation it can update
APP_ID = "io.github.txp666.FluentHMIStudio"  # the macOS bundle id; Studio gives it as its desktop file name

# Plugin folders an application may load on Linux. generic is left out: Qt loads its input plugins only when told
# to (-plugin, QT_QPA_GENERIC_PLUGINS), and eglfs and linuxfb read evdev and libinput themselves.
LINUX_PLUGIN_DIRS = ("platforms", "wayland-shell-integration", "wayland-graphics-integration-client",
                     "wayland-decoration-client", "xcbglintegrations", "egldeviceintegrations", "platformthemes",
                     "platforminputcontexts", "imageformats", "iconengines", "tls", "networkinformation")
# A desktop runs xcb (or wayland), a board without one eglfs or linuxfb. An exported application gets no offscreen or
# minimal: without its window plugin it must not start on one of them unnoticed.
LINUX_PLATFORMS = ("libqxcb.so", "libqwayland", "libqeglfs.so", "libqlinuxfb.so")
# System libraries Qt's xcb plugin needs that a board without apt may lack. They go into lib/ beside Qt's, and so
# into every exported application. libxkbcommon-x11 only works with the libxkbcommon of its own release, so the two
# go together: a newer system libxkbcommon (Ubuntu 26.04's 1.13) crashes the bundled -x11 one at startup.
LINUX_BUNDLED_SYSTEM_LIBS = ("libxcb-cursor.so.", "libxcb-icccm.so.", "libxcb-image.so.", "libxcb-keysyms.so.",
                             "libxcb-render-util.so.", "libxcb-util.so.", "libxkbcommon-x11.so.", "libxkbcommon.so.")
# Everything else Qt loads from the system, as Ubuntu 24.04 and Debian 13 name the packages. The README lists them,
# and the release workflow's clean container has only these, which proves an export needs nothing more.
LINUX_SYSTEM_PACKAGES = (
    "libgl1", "libopengl0", "libegl1", "libfontconfig1", "libfreetype6", "libglib2.0-0t64", "libdbus-1-3",
    "libudev1", "libssl3t64", "libgssapi-krb5-2", "libxkbcommon0", "libx11-6", "libx11-xcb1", "libxext6",
    "libxfixes3", "libxi6", "libxrender1", "libsm6", "libice6", "libxcb1", "libxcb-glx0", "libxcb-randr0",
    "libxcb-render0", "libxcb-shape0", "libxcb-shm0", "libxcb-sync1", "libxcb-xfixes0", "libxcb-xinerama0",
    "libxcb-xinput0", "libxcb-xkb1", "libwayland-client0", "libwayland-cursor0", "libwayland-egl1")
# The studio's package needs those too, apt and pkexec to install its updates, and the glibc of Ubuntu 24.04, where
# it is built. Like Fluent Serial Assistant's it also names the xcb helper libraries; the copies in lib/ are for
# exports, which run where nothing was installed.
DEB_DEPENDS = ("apt", "pkexec", "libc6 (>= 2.39)", "libstdc++6", "libgcc-s1") + LINUX_SYSTEM_PACKAGES + (
    "libxcb-cursor0", "libxcb-icccm4", "libxcb-image0", "libxcb-keysyms1", "libxcb-render-util0", "libxcb-util1",
    "libxkbcommon-x11-0")
# FluentQtWidgets' frameless title bar is proven on X11, which Wayland desktops run through XWayland.
LAUNCHER = f"""#!/bin/sh
if [ -z "${{QT_QPA_PLATFORM:-}}" ]; then
    QT_QPA_PLATFORM=xcb
    export QT_QPA_PLATFORM
fi
exec {INSTALL_ROOT}/FluentHMIStudio "$@"
"""


def elf_files(folder):
    for path in sorted(folder.rglob("*")):
        if path.is_symlink() or not path.is_file():
            continue
        with open(path, "rb") as stream:
            if stream.read(4) == b"\x7fELF":
                yield path


def elf_runpath(path):
    return subprocess.run(["patchelf", "--print-rpath", str(path)], capture_output=True, text=True,
                          check=True).stdout.strip()


def ldd(path):
    """Every library a binary loads, its dependencies' included: soname -> path on this machine, or None."""
    out = subprocess.run(["ldd", str(path)], capture_output=True, text=True, env=user_env()).stdout
    found = re.finditer(r"^\s*(\S+) => (?:(/\S+) \(0x|not found)", out, re.MULTILINE)
    return {match.group(1): match.group(2) for match in found}


def copy_linux_plugins(qt_plugins, target):
    """Copies the plugins of LINUX_PLUGIN_DIRS that Qt has; returns their sources and paths in the package."""
    copied = []
    for folder in LINUX_PLUGIN_DIRS:
        for source in sorted((qt_plugins / folder).glob("*.so")):
            if folder == "platforms" and not source.name.startswith(LINUX_PLATFORMS):
                continue
            (target / folder).mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target / folder / source.name)
            copied.append((source, f"plugins/{folder}/{source.name}"))
    if not any(rel.startswith("plugins/platforms/libqxcb") for _, rel in copied):
        sys.exit(f"no xcb platform plugin in {qt_plugins}")
    return copied


def bundle_linux_libs(binaries, lib_dir, qt_prefix):
    """Copies into lib_dir, by soname, what the binaries load from Qt and the system libraries listed above.

    Returns those sonames. The binaries are the build's and Qt's own, whose RUNPATHs still find Qt in its prefix.
    """
    needed = {}
    for binary in binaries:
        needed.update(ldd(binary))
    names = []
    for soname, resolved in sorted(needed.items()):
        listed = soname.startswith(LINUX_BUNDLED_SYSTEM_LIBS)
        if resolved is None:
            if listed:
                sys.exit(f"{soname} is not installed here, and the package carries it")
            # Needed by a plugin whose system library is missing here (GTK for gtk3). The plugin still ships, and
            # Qt skips it wherever that library is missing too.
            print(f"not on this machine, left to the system: {soname}")
            continue
        if listed or Path(resolved).resolve().is_relative_to(qt_prefix):
            if not (lib_dir / soname).exists():
                lib_dir.mkdir(exist_ok=True)
                shutil.copy2(Path(resolved).resolve(), lib_dir / soname)
            names.append(soname)
    return names


def fix_linux_runpaths(stage):
    """Point each bundled library at its own folder and each plugin at lib/.

    Qt's libraries and plugins come with $ORIGIN and $ORIGIN/../../lib. The system libraries bundled with them
    come with none, and a RUNPATH only serves the object that carries it, not the libraries that object loads.
    """
    for path in elf_files(stage / "lib"):
        if elf_runpath(path) != "$ORIGIN":
            run(["patchelf", "--set-rpath", "$ORIGIN", path])
    for path in elf_files(stage / "plugins"):
        if "$ORIGIN/../../lib" not in elf_runpath(path).split(":"):
            run(["patchelf", "--set-rpath", "$ORIGIN/../../lib", path])


def linux_linked_outside(stage, qt_prefix):
    """Binaries with a RUNPATH outside the package, or that miss a library Qt has."""
    offenders = []
    for path in elf_files(stage):
        outside = [entry for entry in elf_runpath(path).split(":") if entry and not entry.startswith("$ORIGIN")]
        for soname, resolved in ldd(path).items():
            if resolved is None and (qt_prefix / "lib" / soname).exists():
                outside.append(f"{soname} not found")
            elif resolved and Path(resolved).resolve().is_relative_to(qt_prefix):
                outside.append(resolved)
        if outside:
            offenders.append(f"{path.relative_to(stage)}: {', '.join(outside)}")
    return offenders


def linux_loaded_outside(cmd, root, qt_prefix):
    """Libraries a command loads from the build machine's Qt, or from anywhere but the system and root.

    The static checks only see what binaries name; this also catches a qt.conf that misses and lets Qt fall back
    to the plugins of the Qt it was built with.
    """
    print("+", " ".join(str(c) for c in cmd), flush=True)
    result = subprocess.run([str(c) for c in cmd], env=user_env(LD_DEBUG="libs"), capture_output=True, text=True,
                            timeout=CHECK_TIMEOUT)
    if result.returncode != 0:
        sys.exit(f"exit {result.returncode}:\n{result.stdout}{result.stderr[-20000:]}")
    inside = os.path.realpath(root) + os.sep
    forbidden = os.path.realpath(qt_prefix) + os.sep
    loaded = set(re.findall(r"^\s*\d+:\s+calling init: (/.+)$", result.stderr, re.MULTILINE))
    if not loaded:
        sys.exit("LD_DEBUG printed nothing; cannot tell where libraries load from")
    return sorted(lib for lib in loaded if os.path.realpath(lib).startswith(forbidden) or not (
        os.path.realpath(lib).startswith(inside) or lib.startswith(("/lib", "/usr/lib", "/usr/local/lib"))))


def linux_run_inside(cmd, qt_prefix):
    """Run a packaged program, failing if it loads anything from outside its own folder and the system."""
    program = Path(cmd[0])  # FluentHMIStudio in the installation, or an export's start.sh
    outside = linux_loaded_outside(cmd, program.parent, qt_prefix)
    if outside:
        sys.exit(f"{program.name} loads libraries from outside its package:\n  " + "\n  ".join(outside))


def linux_exported_runtime(archive):
    """An exported application is <name>.tar.gz; unpacked beside it as a user would, it starts with start.sh."""
    run(["tar", "-xzf", archive, "-C", archive.parent])
    return archive.parent / archive.name[:-len(".tar.gz")] / "start.sh"


def build_deb(stage, deb, version, architecture):
    """The staged folder as a .deb that installs it in INSTALL_ROOT, with a launcher, a desktop entry and an icon."""
    root = deb.with_suffix("")
    shutil.rmtree(root, ignore_errors=True)
    shutil.copytree(stage, root / INSTALL_ROOT.lstrip("/"), symlinks=True)
    launcher = root / "usr" / "bin" / DEB_PACKAGE
    launcher.parent.mkdir(parents=True)
    launcher.write_text(LAUNCHER, encoding="utf-8")
    launcher.chmod(0o755)
    applications = root / "usr" / "share" / "applications"
    applications.mkdir(parents=True)
    shutil.copy2(ROOT / "resources" / "linux" / f"{APP_ID}.desktop", applications)
    icons = root / "usr" / "share" / "icons" / "hicolor" / "512x512" / "apps"  # resources/logo.png is 512 px
    icons.mkdir(parents=True)
    shutil.copy2(ROOT / "resources" / "logo.png", icons / f"{APP_ID}.png")
    installed = sum(path.stat().st_size for path in root.rglob("*") if path.is_file() and not path.is_symlink())
    (root / "DEBIAN").mkdir()
    (root / "DEBIAN" / "control").write_text(f"""Package: {DEB_PACKAGE}
Version: {version}
Architecture: {architecture}
Maintainer: txp <771454616@qq.com>
Installed-Size: {(installed + 1023) // 1024}
Section: electronics
Priority: optional
Homepage: https://github.com/txp666/FluentHMI-Studio
Depends: {", ".join(DEB_DEPENDS)}
Description: Visual HMI designer for hardware engineers
 FluentHMI Studio designs HMI screens for serial, TCP, MQTT, UDP and Modbus
 devices, previews them live and exports them as standalone applications.
""", encoding="utf-8")
    run(["dpkg-deb", "--root-owner-group", "--build", root, deb])
    shutil.rmtree(root)


def package_linux(build_dir, output_dir, version, config):
    if not (os.environ.get("DISPLAY") or os.environ.get("WAYLAND_DISPLAY")):
        sys.exit("the self-check starts the packaged programs, which needs a display: run under xvfb-run -a")
    qt_prefix = qt_bin_dir(build_dir).parent.resolve()
    arch, deb_arch = {"x86_64": ("x64", "amd64"), "aarch64": ("arm64", "arm64")}.get(platform.machine(), (None, None))
    if not arch:
        sys.exit(f"no Linux package for {platform.machine()}")
    stage = output_dir / "stage" / "FluentHMIStudio"
    shutil.rmtree(stage.parent, ignore_errors=True)
    stage.mkdir(parents=True)

    runtime = find_built(build_dir, "FluentHMIRuntime", config)
    studio = find_built(build_dir, "FluentHMIStudio", config)
    plugins = copy_linux_plugins(qt_prefix / "plugins", stage / "plugins")
    # The runtime's libraries first, as on Windows: runtime-files.txt lists what the runtime takes, so an exported
    # application copies those files and leaves the studio's own behind.
    runtime_libs = bundle_linux_libs([runtime] + [source for source, _ in plugins], stage / "lib", qt_prefix)
    runtime_files = ["FluentHMIRuntime", "qt.conf"] + [rel for _, rel in plugins] + [f"lib/{n}" for n in runtime_libs]
    # The studio's own: offscreen, for --export where there is no display (a board over SSH).
    offscreen = qt_prefix / "plugins" / "platforms" / "libqoffscreen.so"
    shutil.copy2(offscreen, stage / "plugins" / "platforms")
    bundle_linux_libs([studio, offscreen], stage / "lib", qt_prefix)
    for program in (runtime, studio):
        shutil.copy2(program, stage / program.name)
        run(["patchelf", "--set-rpath", "$ORIGIN/lib", stage / program.name])
    fix_linux_runpaths(stage)
    # Qt would also find plugins/ from where its own library is; qt.conf states the layout instead of relying on it.
    (stage / "qt.conf").write_text("[Paths]\nPrefix = .\nLibraries = lib\nPlugins = plugins\n", encoding="utf-8")
    print(f"the runtime takes {len(runtime_files)} files")
    (stage / "runtime-files.txt").write_text("\n".join(runtime_files) + "\n", encoding="utf-8")
    shutil.copytree(ROOT / "examples", stage / "examples")
    shutil.copy2(ROOT / "LICENSE", stage / "LICENSE.txt")

    offenders = linux_linked_outside(stage, qt_prefix)
    for program in ("FluentHMIRuntime", "FluentHMIStudio"):
        offenders += [f"{program}: {soname} not found" for soname, path in ldd(stage / program).items() if not path]
    if offenders:
        sys.exit("not self-contained:\n  " + "\n  ".join(offenders))

    # Built beside the stage and moved to output_dir once it has passed its check.
    deb = stage.parent / f"FluentHMI-Studio-v{version}-linux-{arch}.deb"
    build_deb(stage, deb, version, deb_arch)
    # The package as apt would install it: the studio in it exports the demo, and the export runs from its archive.
    with tempfile.TemporaryDirectory() as work:
        installed = Path(work) / "root"
        run(["dpkg-deb", "-x", deb, installed])
        check_export(installed / INSTALL_ROOT.lstrip("/") / "FluentHMIStudio", linux_exported_runtime, work,
                     lambda cmd: linux_run_inside(cmd, qt_prefix))

    (output_dir / deb.name).unlink(missing_ok=True)
    return Path(shutil.move(str(deb), str(output_dir / deb.name)))


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
    elif sys.platform.startswith("linux"):
        archive = package_linux(build_dir, output_dir, version, args.config)
    else:
        sys.exit("packaging is provided for macOS, Windows and Linux")
    checksum = sha256_file(archive)
    shutil.rmtree(output_dir / "stage", ignore_errors=True)
    print(f"packaged {archive}\nchecksum {checksum}")


if __name__ == "__main__":
    main()
