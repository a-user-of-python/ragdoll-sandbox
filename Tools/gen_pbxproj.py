#!/usr/bin/env python3
"""Generate RagdollSandbox.xcodeproj/project.pbxproj by scanning the repo.

Re-run after each module lands:
    python3 Tools/gen_pbxproj.py

Scans for sources/resources under App/, Renderer/, Game/, Engine/,
Scripting/ and ThirdParty/lua/src (excluding lua.c/luac.c). Deterministic
IDs (md5 of path), so regenerating is stable.

All original code.
"""
import hashlib
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROJDIR = os.path.join(ROOT, "RagdollSandbox.xcodeproj")


def fid(path):
    """Deterministic 24-hex Xcode-style ID."""
    return hashlib.md5(path.encode()).hexdigest()[:24].upper()


def q(s):
    """Quote an OpenStep plist string if needed."""
    if not s:
        return '""'
    ok = all(c.isalnum() or c in "_-./$()" for c in s)
    if ok and not s[0].isdigit():
        return s
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def collect():
    """Return (sources, resources, headers) as lists of repo-relative paths."""
    sources, resources, headers = [], [], []
    src_exts = {".swift", ".c", ".cpp", ".m", ".metal"}
    # Never put host-test code or build artifacts in the iOS app target:
    # tests/ have their own mains and stub headers that would collide.
    skip_dirs = {"build", "tests", ".git"}
    for base in ["App", "Renderer", "Game", "Engine", "Scripting"]:
        d = os.path.join(ROOT, base)
        if not os.path.isdir(d):
            continue
        for dirpath, dirnames, filenames in os.walk(d):
            dirnames[:] = [x for x in dirnames if x not in skip_dirs]
            if any(part in skip_dirs for part in
                   os.path.relpath(dirpath, ROOT).split(os.sep)):
                continue
            for fn in sorted(filenames):
                rel = os.path.relpath(os.path.join(dirpath, fn), ROOT)
                ext = os.path.splitext(fn)[1].lower()
                if ext in src_exts:
                    # Skip Xcode-owned non-sources.
                    if fn.endswith(".bridging-header.h"):
                        continue
                    sources.append(rel)
                elif ext == ".h":
                    headers.append(rel)
                elif ext == ".xcassets" or ext == ".lua":
                    resources.append(rel)
                elif fn == "Contents.json":
                    pass  # inside asset catalogs
    # Lua: only if vendored; exclude the standalone interpreters.
    luadir = os.path.join(ROOT, "ThirdParty", "lua", "src")
    if os.path.isdir(luadir):
        for fn in sorted(os.listdir(luadir)):
            if fn.endswith(".c") and fn not in ("lua.c", "luac.c"):
                sources.append(os.path.join("ThirdParty", "lua", "src", fn))
    # Asset catalogs are directories.
    for base in ["App"]:
        d = os.path.join(ROOT, base)
        if os.path.isdir(d):
            for dirpath, dirnames, _ in os.walk(d):
                for dn in sorted(dirnames):
                    if dn.endswith(".xcassets"):
                        resources.append(os.path.relpath(os.path.join(dirpath, dn), ROOT))
    # Dedupe, keep order.
    def dedup(xs):
        seen, out = set(), []
        for x in xs:
            if x not in seen:
                seen.add(x)
                out.append(x)
        return out
    return dedup(sources), dedup(resources), dedup(headers)


FILETYPE = {
    ".swift": "sourcecode.swift",
    ".c": "sourcecode.c.c",
    ".cpp": "sourcecode.cpp.cpp",
    ".m": "sourcecode.c.objc",
    ".metal": "sourcecode.metal",
    ".h": "sourcecode.c.h",
    ".lua": "text",
    ".xcassets": "folder.assetcatalog",
}


def filetype_for(path):
    if path.endswith(".xcassets"):
        return FILETYPE[".xcassets"]
    return FILETYPE.get(os.path.splitext(path)[1].lower(), "text")


def main():
    sources, resources, headers = collect()
    print(f"sources={len(sources)} resources={len(resources)} headers={len(headers)}")
    for s in sources:
        if not os.path.exists(os.path.join(ROOT, s)):
            print(f"  WARNING: referenced but missing: {s}", file=sys.stderr)

    L = []
    w = L.append

    # ---- IDs ----
    pid_project = fid("project")
    pid_target = fid("target:RagdollSandbox")
    pid_app_ref = fid("product:RagdollSandbox.app")
    cfglist_proj = fid("cfglist:project")
    cfglist_target = fid("cfglist:target")
    cfg_proj_dbg = fid("cfg:project:Debug")
    cfg_proj_rel = fid("cfg:project:Release")
    cfg_tgt_dbg = fid("cfg:target:Debug")
    cfg_tgt_rel = fid("cfg:target:Release")
    phase_sources = fid("phase:sources")
    phase_frameworks = fid("phase:frameworks")
    phase_resources = fid("phase:resources")
    group_main = fid("group:main")
    group_products = fid("group:Products")

    group_ids = {}
    for g in ["App", "Renderer", "Game", "Engine", "Scripting", "ThirdParty"]:
        group_ids[g] = fid("group:" + g)

    w("// !$*UTF8*$!")
    w("{")
    w("\tarchiveVersion = 1;")
    w("\tclasses = {")
    w("\t};")
    w("\tobjectVersion = 77;")
    w("\tobjects = {")

    def begin(pid, isa):
        w(f"\t\t{pid} = {{")
        w(f"\t\t\tisa = {isa};")

    def end():
        w("\t\t};")

    # ---- PBXFileReference ----
    all_files = sources + resources + headers
    for f in all_files:
        fpid = fid("file:" + f)
        name = os.path.basename(f)
        begin(fpid, "PBXFileReference")
        w(f"\t\t\texplicitFileType = {q(filetype_for(f))};")
        w(f"\t\t\tpath = {q(f)};")
        w(f"\t\t\tsourceTree = SOURCE_ROOT;")
        end()
    # Product ref
    begin(pid_app_ref, "PBXFileReference")
    w("\t\t\texplicitFileType = wrapper.application;")
    w("\t\t\tincludeInIndex = 0;")
    w("\t\t\tpath = RagdollSandbox.app;")
    w("\t\t\tsourceTree = BUILT_PRODUCTS_DIR;")
    end()
    # System frameworks
    fw_ids = {}
    for fw in ["Metal.framework", "MetalKit.framework"]:
        fwid = fid("fw:" + fw)
        fw_ids[fw] = fwid
        begin(fwid, "PBXFileReference")
        w("\t\t\tlastKnownFileType = wrapper.framework;")
        w(f"\t\t\tname = {fw};")
        w(f"\t\t\tpath = System/Library/Frameworks/{fw};")
        w("\t\t\tsourceTree = SDKROOT;")
        end()

    # ---- PBXBuildFile ----
    build_ids = {}
    for f in sources:
        bpid = fid("build:" + f)
        build_ids[f] = bpid
        begin(bpid, "PBXBuildFile")
        w(f"\t\t\tfileRef = {fid('file:' + f)};")
        end()
    for f in resources:
        bpid = fid("build:" + f)
        build_ids[f] = bpid
        begin(bpid, "PBXBuildFile")
        w(f"\t\t\tfileRef = {fid('file:' + f)};")
        end()
    for fw in fw_ids:
        bpid = fid("buildfw:" + fw)
        build_ids["fw:" + fw] = bpid
        begin(bpid, "PBXBuildFile")
        w(f"\t\t\tfileRef = {fw_ids[fw]};")
        end()

    # ---- Build phases ----
    begin(phase_sources, "PBXSourcesBuildPhase")
    w("\t\t\tbuildActionMask = 2147483647;")
    w("\t\t\tfiles = (")
    for f in sources:
        w(f"\t\t\t\t{build_ids[f]},")
    w("\t\t\t);")
    w("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
    end()

    begin(phase_frameworks, "PBXFrameworksBuildPhase")
    w("\t\t\tbuildActionMask = 2147483647;")
    w("\t\t\tfiles = (")
    for fw in fw_ids:
        w(f"\t\t\t\t{build_ids['fw:' + fw]},")
    w("\t\t\t);")
    w("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
    end()

    begin(phase_resources, "PBXResourcesBuildPhase")
    w("\t\t\tbuildActionMask = 2147483647;")
    w("\t\t\tfiles = (")
    for f in resources:
        w(f"\t\t\t\t{build_ids[f]},")
    w("\t\t\t);")
    w("\t\t\trunOnlyForDeploymentPostprocessing = 0;")
    end()

    # ---- Groups ----
    def group(gid, name, children, path=None):
        begin(gid, "PBXGroup")
        w("\t\t\tchildren = (")
        for c in children:
            w(f"\t\t\t\t{c},")
        w("\t\t\t);")
        w(f"\t\t\tname = {q(name)};")
        if path:
            w(f"\t\t\tpath = {q(path)};")
        w("\t\t\tsourceTree = \"<group>\";")
        end()

    for g, gid in group_ids.items():
        members = [fid("file:" + f) for f in all_files
                   if f == g or f.startswith(g + "/")]
        group(gid, g, members, path=g)
    group(group_products, "Products", [pid_app_ref])
    group(group_main, None or "RagdollSandbox",
          [group_ids[g] for g in ["App", "Renderer", "Game", "Engine",
                                 "Scripting", "ThirdParty"]] + [group_products])

    # ---- Target ----
    begin(pid_target, "PBXNativeTarget")
    w(f"\t\t\tbuildConfigurationList = {cfglist_target};")
    w("\t\t\tbuildPhases = (")
    w(f"\t\t\t\t{phase_sources},")
    w(f"\t\t\t\t{phase_frameworks},")
    w(f"\t\t\t\t{phase_resources},")
    w("\t\t\t);")
    w("\t\t\tbuildRules = (")
    w("\t\t\t);")
    w("\t\t\tdependencies = (")
    w("\t\t\t);")
    w('\t\t\tname = RagdollSandbox;')
    w(f"\t\t\tproductName = RagdollSandbox;")
    w(f"\t\t\tproductReference = {pid_app_ref};")
    w("\t\t\tproductType = \"com.apple.product-type.application\";")
    end()

    # ---- Configurations ----
    def xcconfig(pid, name, settings):
        begin(pid, "XCBuildConfiguration")
        w("\t\t\tbuildSettings = {")
        for k, v in settings.items():
            w(f"\t\t\t\t{k} = {v};")
        w("\t\t\t};")
        w(f"\t\t\tname = {name};")
        end()

    def xcconfiglist(pid, cfgs):
        begin(pid, "XCConfigurationList")
        w("\t\t\tbuildConfigurations = (")
        for c in cfgs:
            w(f"\t\t\t\t{c},")
        w("\t\t\t);")
        w("\t\t\tdefaultConfigurationIsVisible = 0;")
        w("\t\t\tdefaultConfigurationName = Release;")
        end()

    proj_common = {
        "ALWAYS_SEARCH_USER_PATHS": "NO",
        "CLANG_ANALYZER_NONNULL": "YES",
        "CLANG_CXX_LANGUAGE_STANDARD": '"c++17"',
        "CLANG_CXX_LIBRARY": '"libc++"',
        "CLANG_WARN_QUOTED_INCLUDE_IN_FRAMEWORK_HEADER": "YES",
        "CURRENT_PROJECT_VERSION": "1",
        "DEBUG_INFORMATION_FORMAT": '"dwarf-with-dsym"',
        "ENABLE_BITCODE": "NO",
        "GCC_C_LANGUAGE_STANDARD": "gnu17",
        "IPHONEOS_DEPLOYMENT_TARGET": "17.0",
        "MARKETING_VERSION": "1.0",
        "SDKROOT": "iphoneos",
        "SWIFT_VERSION": "5.0",
        "TARGETED_DEVICE_FAMILY": '"2"',
    }
    proj_dbg = dict(proj_common, **{"GCC_OPTIMIZATION_LEVEL": "0",
                                    "MTL_ENABLE_DEBUG_INFO": "INCLUDE_SOURCE",
                                    "ONLY_ACTIVE_ARCH": "YES",
                                    "SWIFT_OPTIMIZATION_LEVEL": '"-Onone"'})
    proj_rel = dict(proj_common, **{"MTL_ENABLE_DEBUG_INFO": "NO",
                                    "SWIFT_OPTIMIZATION_LEVEL": '"-O"'})

    xcconfig(cfg_proj_dbg, "Debug", proj_dbg)
    xcconfig(cfg_proj_rel, "Release", proj_rel)
    xcconfiglist(cfglist_proj, [cfg_proj_dbg, cfg_proj_rel])

    tgt_common = {
        "ASSETCATALOG_COMPILER_APPICON_NAME": "AppIcon",
        "CODE_SIGN_ENTITLEMENTS": '"App/RagdollSandbox.entitlements"',
        "CODE_SIGN_STYLE": "Automatic",
        "DEVELOPMENT_TEAM": '""',
        "HEADER_SEARCH_PATHS": '"$(SRCROOT)/Game $(SRCROOT)/ThirdParty/lua/src $(SRCROOT)/Scripting"',
        "INFOPLIST_FILE": '"App/Info.plist"',
        "LD_RUNPATH_SEARCH_PATHS": '"$(inherited) @executable_path/Frameworks"',
        "PRODUCT_BUNDLE_IDENTIFIER": '"com.ragdollsandbox.app"',
        "PRODUCT_NAME": '"$(TARGET_NAME)"',
        "SWIFT_OBJC_BRIDGING_HEADER": '"App/RagdollSandbox-Bridging-Header.h"',
    }
    xcconfig(cfg_tgt_dbg, "Debug", tgt_common)
    xcconfig(cfg_tgt_rel, "Release", tgt_common)
    xcconfiglist(cfglist_target, [cfg_tgt_dbg, cfg_tgt_rel])

    # ---- Project ----
    begin(pid_project, "PBXProject")
    w("\t\t\tattributes = {")
    w("\t\t\t\tBuildIndependentTargetsInParallel = 1;")
    w("\t\t\t\tLastUpgradeCheck = 1600;")
    w("\t\t\t};")
    w(f"\t\t\tbuildConfigurationList = {cfglist_proj};")
    w("\t\t\tcompatibilityVersion = \"Xcode 15.0\";")
    w("\t\t\tdevelopmentRegion = en;")
    w("\t\t\thasScannedForEncodings = 0;")
    w("\t\t\tknownRegions = (")
    w("\t\t\t\ten,")
    w("\t\t\t);")
    w(f"\t\t\tmainGroup = {group_main};")
    w(f"\t\t\tproductRefGroup = {group_products};")
    w("\t\t\tprojectDirPath = \"\";")
    w("\t\t\tprojectRoot = \"\";")
    w("\t\t\ttargets = (")
    w(f"\t\t\t\t{pid_target},")
    w("\t\t\t);")
    end()

    w("\t};")
    w(f"\trootObject = {pid_project};")
    w("}")

    os.makedirs(PROJDIR, exist_ok=True)
    out = os.path.join(PROJDIR, "project.pbxproj")
    with open(out, "w") as f:
        f.write("\n".join(L) + "\n")
    print("wrote", out)


if __name__ == "__main__":
    main()
