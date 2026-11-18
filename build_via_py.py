import os, subprocess, glob, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
VC = r"D:\vc\VC\Tools\MSVC\14.51.36231"
SDK = r"D:\Windows Kits\10"
SDKVER = "10.0.26100.0"
RAY = os.path.join(ROOT, "libs", "raylib-win64")
STB = os.path.join(ROOT, "libs", "stb")
XML = os.path.join(ROOT, "libs", "tinyxml2")
OUT = os.path.join(ROOT, "out")
OBJ = os.path.join(OUT, "obj")

os.makedirs(OBJ, exist_ok=True)

env = os.environ.copy()
include = [
    os.path.join(VC, "include"),
    os.path.join(SDK, "Include", SDKVER, "um"),
    os.path.join(SDK, "Include", SDKVER, "ucrt"),
    os.path.join(SDK, "Include", SDKVER, "shared"),
    os.path.join(SDK, "Include", SDKVER, "winrt"),
    os.path.join(SDK, "Include", SDKVER, "cppwinrt"),
    RAY + r"\include", STB, XML,
]
lib = [
    os.path.join(VC, "lib", "x64"),
    os.path.join(SDK, "Lib", SDKVER, "ucrt", "x64"),
    os.path.join(SDK, "Lib", SDKVER, "um", "x64"),
]
env["INCLUDE"] = ";".join(include)
env["LIB"] = ";".join(lib)
env["PATH"] = os.path.join(VC, "bin", "Hostx64", "x64") + ";" + env.get("PATH", "")

cl = os.path.join(VC, "bin", "Hostx64", "x64", "cl.exe")
srcs = sorted(glob.glob(os.path.join(ROOT, "src", "*.cpp")))
xmlcpp = os.path.join(XML, "tinyxml2.cpp")
if os.path.exists(xmlcpp):
    srcs.append(xmlcpp)

cmd = [cl, "/nologo", "/std:c++17", "/EHsc", "/MD", "/O2", "/DNDEBUG",
       "/I" + os.path.join(RAY, "include"), "/I" + STB, "/I" + XML,
       "/Fo" + OBJ + os.sep,
       *srcs,
       "/link", "/SUBSYSTEM:WINDOWS", "/ENTRY:mainCRTStartup",
       os.path.join(RAY, "lib", "raylib.lib"),
       "winmm.lib", "opengl32.lib", "gdi32.lib", "user32.lib", "shell32.lib",
       "/OUT:" + os.path.join(OUT, "rp.exe")]

print("Compiling", len(srcs), "sources...")
r = subprocess.run(cmd, cwd=ROOT, env=env)
sys.exit(r.returncode)
